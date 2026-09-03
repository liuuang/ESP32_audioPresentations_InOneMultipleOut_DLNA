// ============================================================
// control.cpp — 控制面实现（Web + UPnP/DLNA + 自动播放）
// ============================================================
#include "control.h"
#include <ESPmDNS.h>
#include <esp_log.h>

ControlPanel* g_self = NULL;   // WebServer 回调需要全局指针
static uint32_t s_ssdpResp = 0;   // SSDP 响应计数（只按 x20 打印，避免刷屏阻塞解码）

// 关掉 WebServer 的 "request handler not found" 刷屏（log_e 无法被 onNotFound 拦截，
// 局域网扫描器扫 80 端口时会每秒打几十条 → 阻塞串口 → 解码一卡一卡）
static struct SuppressWsLog {
  SuppressWsLog() { esp_log_level_set("WEBSERVER", ESP_LOG_NONE); }
} s_suppressWsLog;

// 注意: 不能用 PROGMEM! snprintf 无法直接读 flash, 会返回乱码
const char g_descTmpl[] = R"XML(
<root xmlns="urn:schemas-upnp-org:device-1-0">
  <specVersion><major>1</major><minor>0</minor></specVersion>
  <device>
    <deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>
    <friendlyName>%s</friendlyName>
    <manufacturer>DIY</manufacturer>
    <modelName>ESP32_Multiroom</modelName>
    <UDN>uuid:11223344-5566-7788-9900-AABBCCDDEEFF</UDN>
    <serviceList>
      <service>
        <serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>
        <serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>
        <controlURL>/AVTransport/Control</controlURL>
        <eventSubURL>/AVTransport/Event</eventSubURL>
        <SCPDURL>/AVTransport/scpd.xml</SCPDURL>
      </service>
      <service>
        <serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>
        <serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>
        <controlURL>/ConnectionManager/Control</controlURL>
        <eventSubURL>/ConnectionManager/Event</eventSubURL>
        <SCPDURL>/ConnectionManager/scpd.xml</SCPDURL>
      </service>
      <service>
        <serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>
        <serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>
        <controlURL>/RenderingControl/Control</controlURL>
        <eventSubURL>/RenderingControl/Event</eventSubURL>
        <SCPDURL>/RenderingControl/scpd.xml</SCPDURL>
      </service>
    </serviceList>
  </device>
</root>
)XML";

void ControlPanel::begin(const char* friendlyName) {
  m_name = friendlyName;
  g_self = this;

  m_server.on("/", [this]() { handleRoot(); });
  m_server.on("/play", HTTP_POST, [this]() { handlePlay(); });
  m_server.on("/stop", HTTP_POST, [this]() { handleStop(); });
  m_server.on("/slaves", [this]() { handleSlaves(); });
  m_server.on("/slave/ctl", HTTP_POST, [this]() { handleSlaveCtl(); });
  m_server.on("/desc.xml", [this]() { handleDesc(); });
  // 标准 UPnP 三服务描述（控制点会逐个请求）
  m_server.on("/AVTransport/scpd.xml", [this]() { handleSCPD(); });
  m_server.on("/ConnectionManager/scpd.xml", [this]() { handleSCPD(); });
  m_server.on("/RenderingControl/scpd.xml", [this]() { handleSCPD(); });
  m_server.on("/AVTransport/Control", HTTP_POST, [this]() { handleAVTransport(); });
  m_server.on("/ConnectionManager/Control", HTTP_POST, [this]() { handleCM(); });
  m_server.on("/RenderingControl/Control", HTTP_POST, [this]() { handleRC(); });
  // GENA 事件订阅（网易云等投屏前必先对 eventSubURL 发 SUBSCRIBE，回 404 会被
  // 判定"不支持事件"而中止推流。必须正确应答 200+SID。同 URL 两方法各注册一次）
  m_server.on("/AVTransport/Event", HTTP_SUBSCRIBE, [this]() { handleEvent(); });
  m_server.on("/AVTransport/Event", HTTP_UNSUBSCRIBE, [this]() { handleEvent(); });
  m_server.on("/ConnectionManager/Event", HTTP_SUBSCRIBE, [this]() { handleEvent(); });
  m_server.on("/ConnectionManager/Event", HTTP_UNSUBSCRIBE, [this]() { handleEvent(); });
  m_server.on("/RenderingControl/Event", HTTP_SUBSCRIBE, [this]() { handleEvent(); });
  m_server.on("/RenderingControl/Event", HTTP_UNSUBSCRIBE, [this]() { handleEvent(); });
  // 兜底：未注册路径静默 404（不打印——刷屏会阻塞串口拖慢解码）
  m_server.onNotFound([]() {
    if (g_self) g_self->m_server.send(404, "text/plain", "Not Found");
  });
  m_server.begin();
  Serial.printf("控制面: 网页 http://%s/\n", WiFi.localIP().toString().c_str());
  Serial.println("控制面: DLNA v3 (CurrentURI解析+GENA订阅)");

  // ⚠️ 必须 beginMulticast 加入 SSDP 组播组(239.255.255.250)，
  // 否则控制点(网易云/BubbleUPnP)发的 M-SEARCH 组播包根本到不了这里，
  // 设备永远搜不到。begin(1900) 只监听单播 = 收不到任何搜索。
  m_udpUpnp.beginMulticast(IPAddress(239, 255, 255, 250), 1900);
  sendNotify();
}

void ControlPanel::loop() {
  m_server.handleClient();
  handleSearch();
  autoPlay();
  // NOTIFY 每 5s 广播一次（主动现身——很多控制点靠收 alive 发现设备，
  // 不一定发 M-SEARCH；30s 太久手机打开 App 时可能已错过）
  if (millis() - m_lastNotify > 5000) {
    m_lastNotify = millis();
    sendNotify();
  }
}

void ControlPanel::setAutoPlayURL(const char* url) {
  m_autoURL = url;
  m_autoPlayed = false;
  m_lastPlayTry = 0;
  Serial.printf("控制面: 待自动播放 %s\n", url);
}

// ============ 自动播放（DNS 就绪后，失败 5s 重试） ============
void ControlPanel::autoPlay() {
  if (m_autoPlayed || m_autoURL.length() == 0 || !m_src) return;
  if (millis() - m_lastPlayTry < 5000) return;
  m_lastPlayTry = millis();
  if (WiFi.status() != WL_CONNECTED) return;
  m_src->play(m_autoURL.c_str());
  m_autoPlayed = true;
  Serial.print("自动播放: "); Serial.println(m_autoURL);
}

// ============ Web 页面 ============
// 主页：播放控制 + 每台从机音量/延迟滑块（一体，不拆子页）
void ControlPanel::handleRoot() {
  String s = (m_src && m_src->isRunning()) ? "▶ 播放中" : "⏹ 已停止";
  String h = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width'><title>ESP32 音响</title>";
  h += "<style>body{font-family:sans-serif;padding:20px;background:#111;color:#eee;max-width:600px;margin:0 auto}"
       "input[type=text]{width:100%;padding:8px;margin:8px 0;box-sizing:border-box;background:#222;color:#eee;border:1px solid #444;border-radius:4px}"
       "button{padding:10px 20px;margin:4px;font-size:15px;background:#2a7fff;color:#fff;border:none;border-radius:4px}"
       ".st{padding:12px;background:#1a1a1a;border-radius:6px;border:1px solid #333}"
       ".sl{border:1px solid #333;border-radius:8px;padding:12px;margin:10px 0;background:#1a1a1a}"
       ".sec{margin-top:16px;font-weight:bold;color:#8ab4ff}"
       "input[type=range]{width:100%;margin:2px 0}"
       "input[type=number]{padding:4px;margin:2px 0;width:100%;box-sizing:border-box;background:#222;color:#eee;border:1px solid #444;border-radius:4px}"
       ".val{font-size:13px;color:#aaa}.row{display:flex;justify-content:space-between;align-items:center}"
       "small{color:#888}</style></head><body>";
  h += "<h1>🎵 ESP32 音响</h1>";
  h += "<div class='st'>" + s + " <small>" + WiFi.localIP().toString() + "</small></div>";

  // 播放控制
  h += "<div class='sec'>播放</div>";
  h += "<form action='/play' method='POST' style='display:flex;gap:6px'>"
       "<input type='text' name='url' placeholder='音乐 URL 或电台流' required style='flex:1'>"
       "<button type='submit'>▶ 播放</button></form>";
  h += "<form action='/stop' method='POST'><button>⏹ 停止</button></form>";

  // 从机控制（直接嵌主页）
  int n = m_dist ? m_dist->slaveCount() : 0;
  h += "<div class='sec'>从机控制 (" + String(n) + ")</div>";
  if (n == 0) {
    h += "<p style='color:#888'>暂无从机接入</p>";
  }
  for (int i = 0; i < n; i++) {
    IPAddress ip = m_dist->slaveIP(i);
    h += "<div class='sl'><div class='row'><b>从机 #" + String(i + 1) + "</b>"
         "<small>" + ip.toString() + "</small></div>";
    h += "<div class='row'><span>音量 <span id='v" + String(i) + "'>100</span>%</span>"
         "<small><a href='javascript:void(0)' onclick='setVol(" + String(i) + ",100)' style='color:#8ab4ff'>100</a> "
         "<a href='javascript:void(0)' onclick='setVol(" + String(i) + ",50)' style='color:#8ab4ff'>50</a> "
         "<a href='javascript:void(0)' onclick='setVol(" + String(i) + ",0)' style='color:#8ab4ff'>0</a></small></div>";
    h += "<input type='range' min='0' max='100' value='100' id='vol" + String(i) + "' "
         "oninput='document.getElementById(\"v" + String(i) + "\").innerHTML=this.value' "
         "onchange='setVol(" + String(i) + ",this.value)'>";
    h += "<div class='row'><span>延迟 <span id='d" + String(i) + "'>0</span> ms</span></div>";
    h += "<input type='number' min='0' max='5000' step='10' value='0' id='dly" + String(i) + "' "
         "oninput='document.getElementById(\"d" + String(i) + "\").innerHTML=this.value' "
         "onchange='setDly(" + String(i) + ",this.value)'>";
    h += "</div>";
  }

  h += "<script>var IPS=[" ;
  for (int i = 0; i < n; i++) {
    if (i) h += ",";
    h += "'" + m_dist->slaveIP(i).toString() + "'";
  }
  h += "];"
       "function setVol(i,v){var ip=IPS[i];if(!ip)return;"
       "fetch('/slave/ctl',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
       "body:'ip='+ip+'&vol='+v});}"
       "function setDly(i,d){var ip=IPS[i];if(!ip)return;"
       "fetch('/slave/ctl',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
       "body:'ip='+ip+'&dly='+d});}"
       "setInterval(function(){location.reload();},30000);</script>";
  h += "</body></html>";
  m_server.send(200, "text/html; charset=utf-8", h);
}

void ControlPanel::handlePlay() {
  String url = m_server.arg("url");
  if (!url.length()) { m_server.send(400, "text/plain", "no url"); return; }
  Serial.println("播放: " + url);
  if (m_src) m_src->play(url.c_str());
  m_server.sendHeader("Location", "/");
  m_server.send(302, "text/plain", "OK");
}

void ControlPanel::handleStop() {
  if (m_src) m_src->stop();
  m_server.sendHeader("Location", "/");
  m_server.send(302, "text/plain", "OK");
}

// 从机控制页：列出所有已注册从机，每台带音量/延迟滑块
void ControlPanel::handleSlaves() {
  int n = m_dist ? m_dist->slaveCount() : 0;
  String h = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width'><title>从机控制</title>"
    "<style>body{font-family:sans-serif;padding:20px;background:#111;color:#eee}.sl{border:1px solid #333;border-radius:8px;padding:14px;margin:10px 0;background:#1a1a1a}"
    "input[type=range]{width:100%}button{padding:8px 16px;margin-top:6px;background:#2a7fff;color:#fff;border:none;border-radius:4px}"
    ".val{font-size:13px;color:#aaa}input[type=number]{padding:4px;margin:4px 0;width:100%}</style></head><body>";
  h += "<h2>从机控制 (" + String(n) + ")</h2>";
  if (n == 0) h += "<p>暂无从机接入</p>";
  for (int i = 0; i < n; i++) {
    IPAddress ip = m_dist->slaveIP(i);
    h += "<div class='sl'><b>从机 #" + String(i + 1) + "</b> <span class='val'>" + ip.toString() + "</span>";
    // 音量滑块（id 带编号 vol0/vol1/...，JS 必须用相同 id）
    h += "<label>音量 <span id='v" + String(i) + "'>100</span>%</label>";
    h += "<input type='range' min='0' max='100' value='100' id='vol" + String(i) + "' oninput='document.getElementById(\"v" + String(i) + "\").innerHTML=this.value'>";
    // 延迟输入
    h += "<label>延迟 <span id='d" + String(i) + "'>0</span> ms</label>";
    h += "<input type='number' min='0' max='5000' step='10' value='0' id='dly" + String(i) + "' oninput='document.getElementById(\"d" + String(i) + "\").innerHTML=this.value'>";
    h += "<br><button onclick='send(\"" + ip.toString() + "\"," + String(i) + ")'>应用</button>";
    h += "</div>";
  }
  h += "<script>function send(ip,idx){"
       "var volEl=document.getElementById('vol'+idx);"
       "var dlyEl=document.getElementById('dly'+idx);"
       "var vol=volEl?volEl.value:100;var dly=dlyEl?dlyEl.value:0;"
       "fetch('/slave/ctl',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
       "body:'ip='+ip+'&vol='+vol+'&dly='+dly});}"
       "</script></body></html>";
  m_server.send(200, "text/html; charset=utf-8", h);
}

// POST /slave/ctl?ip=x&vol=y&dly=z → 向从机发 VOL/DLY 控制消息
void ControlPanel::handleSlaveCtl() {
  String ipStr = m_server.arg("ip");
  String volStr = m_server.arg("vol");
  String dlyStr = m_server.arg("dly");
  IPAddress ip;
  if (!ip.fromString(ipStr) || !m_dist) {
    m_server.send(400, "text/plain", "bad ip");
    return;
  }
  if (volStr.length() > 0) {
    int v = constrain(volStr.toInt(), 0, 100);
    char msg[16];
    snprintf(msg, sizeof(msg), "VOL %d", v);
    m_dist->sendCtrl(ip, msg);
    Serial.printf("[WEB] 从机 %s 音量 %d%%\n", ipStr.c_str(), v);
  }
  if (dlyStr.length() > 0) {
    int d = constrain(dlyStr.toInt(), 0, 5000);
    char msg[16];
    snprintf(msg, sizeof(msg), "DLY %d", d);
    m_dist->sendCtrl(ip, msg);
    Serial.printf("[WEB] 从机 %s 延迟 %dms\n", ipStr.c_str(), d);
  }
  m_server.send(200, "text/plain", "OK");
}

void ControlPanel::handleDesc() {
  // buf 必须够大！三服务描述约 1200B，600 会被截断 → XML 非法 → DLNA 失败
  char buf[2048];
  snprintf(buf, sizeof(buf), g_descTmpl, m_name.c_str());
  m_server.send(200, "text/xml; charset=utf-8", buf);
}

// 标准 AVTransport SCPD（含 DLNA 需要的核心动作）
static const char SCPD_AVT[] PROGMEM =
  "<?xml version=\"1.0\"?>"
  "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
  "<specVersion><major>1</major><minor>0</minor></specVersion>"
  "<actionList>"
  "<action><name>SetAVTransportURI</name>"
  "<argumentList>"
  "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
  "<argument><name>CurrentURI</name><direction>in</direction><relatedStateVariable>AVTransportURI</relatedStateVariable></argument>"
  "<argument><name>CurrentURIMetaData</name><direction>in</direction><relatedStateVariable>AVTransportURIMetaData</relatedStateVariable></argument>"
  "</argumentList></action>"
  "<action><name>Play</name>"
  "<argumentList><argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
  "<argument><name>Speed</name><direction>in</direction><relatedStateVariable>TransportPlaySpeed</relatedStateVariable></argument>"
  "</argumentList></action>"
  "<action><name>Stop</name>"
  "<argumentList><argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
  "</argumentList></action>"
  "<action><name>Pause</name>"
  "<argumentList><argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
  "</argumentList></action>"
  "<action><name>GetTransportInfo</name>"
  "<argumentList><argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
  "<argument><name>CurrentTransportState</name><direction>out</direction><relatedStateVariable>TransportState</relatedStateVariable></argument>"
  "<argument><name>CurrentTransportStatus</name><direction>out</direction><relatedStateVariable>TransportStatus</relatedStateVariable></argument>"
  "<argument><name>CurrentSpeed</name><direction>out</direction><relatedStateVariable>TransportPlaySpeed</relatedStateVariable></argument>"
  "</argumentList></action>"
  "<action><name>GetMediaInfo</name>"
  "<argumentList><argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
  "<argument><name>NrTracks</name><direction>out</direction><relatedStateVariable>NumberOfTracks</relatedStateVariable></argument>"
  "<argument><name>MediaDuration</name><direction>out</direction><relatedStateVariable>CurrentMediaDuration</relatedStateVariable></argument>"
  "<argument><name>CurrentURI</name><direction>out</direction><relatedStateVariable>AVTransportURI</relatedStateVariable></argument>"
  "</argumentList></action>"
  "</actionList>"
  "<serviceStateTable>"
  "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_InstanceID</name><dataType>ui4</dataType></stateVariable>"
  "<stateVariable sendEvents=\"no\"><name>AVTransportURI</name><dataType>string</dataType></stateVariable>"
  "<stateVariable sendEvents=\"no\"><name>AVTransportURIMetaData</name><dataType>string</dataType></stateVariable>"
  "<stateVariable sendEvents=\"no\"><name>TransportPlaySpeed</name><dataType>string</dataType></stateVariable>"
  "<stateVariable sendEvents=\"yes\"><name>TransportState</name><dataType>string</dataType>"
  "<allowedValueList><allowedValue>STOPPED</allowedValue><allowedValue>PLAYING</allowedValue><allowedValue>PAUSED_PLAYBACK</allowedValue></allowedValueList></stateVariable>"
  "<stateVariable sendEvents=\"no\"><name>TransportStatus</name><dataType>string</dataType></stateVariable>"
  "<stateVariable sendEvents=\"no\"><name>NumberOfTracks</name><dataType>ui4</dataType></stateVariable>"
  "<stateVariable sendEvents=\"no\"><name>CurrentMediaDuration</name><dataType>string</dataType></stateVariable>"
  "</serviceStateTable></scpd>";

// ConnectionManager SCPD：含 GetProtocolInfo —— 控制点据此判断渲染器能播什么，缺失=不可用
static const char SCPD_CM[] PROGMEM =
  "<?xml version=\"1.0\"?>"
  "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
  "<specVersion><major>1</major><minor>0</minor></specVersion>"
  "<actionList>"
  "<action><name>GetProtocolInfo</name>"
  "<argumentList>"
  "<argument><name>Source</name><direction>out</direction><relatedStateVariable>SourceProtocolInfo</relatedStateVariable></argument>"
  "<argument><name>Sink</name><direction>out</direction><relatedStateVariable>SinkProtocolInfo</relatedStateVariable></argument>"
  "</argumentList></action>"
  "<action><name>GetCurrentConnectionIDs</name>"
  "<argumentList><argument><name>ConnectionIDs</name><direction>out</direction><relatedStateVariable>CurrentConnectionIDs</relatedStateVariable></argument>"
  "</argumentList></action>"
  "</actionList>"
  "<serviceStateTable>"
  "<stateVariable sendEvents=\"no\"><name>SourceProtocolInfo</name><dataType>string</dataType></stateVariable>"
  "<stateVariable sendEvents=\"no\"><name>SinkProtocolInfo</name><dataType>string</dataType></stateVariable>"
  "<stateVariable sendEvents=\"no\"><name>CurrentConnectionIDs</name><dataType>string</dataType></stateVariable>"
  "</serviceStateTable></scpd>";

// RenderingControl SCPD：音量（BubbleUPnP 拉到渲染器后会立即查音量）
static const char SCPD_RC[] PROGMEM =
  "<?xml version=\"1.0\"?>"
  "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
  "<specVersion><major>1</major><minor>0</minor></specVersion>"
  "<actionList>"
  "<action><name>GetVolume</name>"
  "<argumentList>"
  "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
  "<argument><name>Channel</name><direction>in</direction><relatedStateVariable>Channel</relatedStateVariable></argument>"
  "<argument><name>CurrentVolume</name><direction>out</direction><relatedStateVariable>Volume</relatedStateVariable></argument>"
  "</argumentList></action>"
  "<action><name>SetVolume</name>"
  "<argumentList>"
  "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
  "<argument><name>Channel</name><direction>in</direction><relatedStateVariable>Channel</relatedStateVariable></argument>"
  "<argument><name>DesiredVolume</name><direction>in</direction><relatedStateVariable>Volume</relatedStateVariable></argument>"
  "</argumentList></action>"
  "</actionList>"
  "<serviceStateTable>"
  "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_InstanceID</name><dataType>ui4</dataType></stateVariable>"
  "<stateVariable sendEvents=\"no\"><name>Channel</name><dataType>string</dataType><allowedValueList><allowedValue>Master</allowedValue></allowedValueList></stateVariable>"
  "<stateVariable sendEvents=\"yes\"><name>Volume</name><dataType>ui2</dataType><allowedValueRange><minimum>0</minimum><maximum>100</maximum></allowedValueRange></stateVariable>"
  "</serviceStateTable></scpd>";

// UPnP 时长格式: H:MM:SS（位置同步/时长上报用）
static String upnpTime(uint32_t sec) {
  char t[16];
  snprintf(t, sizeof(t), "%u:%02u:%02u",
           (unsigned)(sec / 3600), (unsigned)((sec / 60) % 60), (unsigned)(sec % 60));
  return String(t);
}

// XML 转义（回显控制点给的 URL 时用——查询串常含 &）
static String xmlEscape(const String& s) {
  String o = s;
  o.replace("&", "&amp;");
  return o;
}

void ControlPanel::handleSCPD() {  const char* xml = NULL;
  if (m_server.uri().indexOf("AVTransport") != -1) xml = SCPD_AVT;
  else if (m_server.uri().indexOf("ConnectionManager") != -1) xml = SCPD_CM;
  else if (m_server.uri().indexOf("RenderingControl") != -1) xml = SCPD_RC;
  if (!xml) { m_server.send(404, "text/plain", "no scpd"); return; }
  // 注意: SCPD_AVT 实测 3550B，曾用 2200 缓冲 → 截断 → XML 非法 → 控制点丢弃设备（看不到）
  static char buf[4200];
  int n = snprintf_P(buf, sizeof(buf), xml);
  if (n >= (int)sizeof(buf))
    Serial.printf("[SCPD] 超长截断 %d>=%u，需加大缓冲!\n", n, (unsigned)sizeof(buf));
  m_server.send(200, "text/xml; charset=utf-8", buf);
}

void ControlPanel::handleAVTransport() {
  String b = m_server.arg("plain");
  bool running = m_src && m_src->isRunning();

  // 各类 SOAP 响应的公共壳
  String action = "SetAVTransportURI";
  String body = "";

  if (b.indexOf("SetAVTransportURI") != -1) {
    // 必须取 <CurrentURI> 标签内的 URL。抓第一个 "http://" 会命中 SOAP 命名空间
    // (http://schemas.xmlsoap.org/soap/envelope/)，把整段 envelope 当音轨地址
    int p = b.indexOf("<CurrentURI>");
    if (p != -1) {
      int start = p + 12;                        // strlen("<CurrentURI>")
      int e = b.indexOf("</CurrentURI>", start);
      if (e != -1) {
        String url = b.substring(start, e);
        url.replace("&amp;", "&");               // XML 转义还原（URL 查询串常含 &）
        url.trim();
        if (url.startsWith("http://") || url.startsWith("https://")) {
          Serial.printf("[AVT] 推流: %s\n", url.c_str());
          m_lastURI = url;
          if (m_src) m_src->play(url.c_str());
        } else {
          Serial.printf("[AVT] CurrentURI 无有效 URL: %s\n", url.c_str());
        }
      }
    }
    action = "SetAVTransportURI";
  } else if (b.indexOf("GetTransportInfo") != -1) {
    action = "GetTransportInfo";
    String state = running ? "PLAYING" : "STOPPED";
    body = "<CurrentTransportState>" + state + "</CurrentTransportState>"
           "<CurrentTransportStatus>OK</CurrentTransportStatus>"
           "<CurrentSpeed>1</CurrentSpeed>";
  } else if (b.indexOf("GetMediaInfo") != -1) {
    action = "GetMediaInfo";
    uint32_t dur = m_src ? m_src->durationSec() : 0;
    body = "<NrTracks>1</NrTracks><MediaDuration>" + upnpTime(dur) + "</MediaDuration>"
           "<CurrentURI>" + xmlEscape(m_lastURI) + "</CurrentURI>";
  } else if (b.indexOf("GetTransportSettings") != -1) {
    action = "GetTransportSettings";
    body = "<PlayMode>NORMAL</PlayMode><RecQualityMode>0</RecQualityMode>";
  } else if (b.indexOf("Play") != -1) {
    action = "Play";
  } else if (b.indexOf("Pause") != -1) {
    action = "Pause";
  } else if (b.indexOf("Stop") != -1) {
    action = "Stop";
    if (m_src) m_src->stop();
  } else if (b.indexOf("GetPositionInfo") != -1) {
    action = "GetPositionInfo";
    // 位置不靠事件推送，控制点每秒轮询 GetPositionInfo 的 RelTime 画进度条。
    // 必须返回真实前进秒数，否则进度钉在 0。
    uint32_t pos = m_src ? m_src->positionSec() : 0;
    uint32_t dur = m_src ? m_src->durationSec() : 0;
    body = "<Track>1</Track><TrackDuration>" + upnpTime(dur) + "</TrackDuration>"
           "<TrackMetaData></TrackMetaData><TrackURI>" + xmlEscape(m_lastURI) + "</TrackURI>"
           "<RelTime>" + upnpTime(pos) + "</RelTime><AbsTime>" + upnpTime(pos) + "</AbsTime>"
           "<RelCount>0</RelCount><AbsCount>0</AbsCount>";
  } else {
    Serial.printf("[AVT] 未知动作\n");
    action = "SetAVTransportURI";   // 兜底回 SetAVTransportURIResponse
  }

  String ns = "urn:schemas-upnp-org:service:AVTransport:1";
  String resp = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + action + "Response xmlns:u=\"" + ns + "\">" + body + "</u:" + action + "Response></s:Body></s:Envelope>";
  m_server.send(200, "text/xml; charset=utf-8", resp);
}

// ============ ConnectionManager SOAP（GetProtocolInfo 等） ============
void ControlPanel::handleCM() {
  String b = m_server.arg("plain");
  String ns = "urn:schemas-upnp-org:service:ConnectionManager:1";
  String action, body;
  if (b.indexOf("GetProtocolInfo") != -1) {
    action = "GetProtocolInfo";
    // Sink = 本机可拉取播放的 http 音频类型（与解码库能力一致，逗号分隔）
    body = "<Source></Source><Sink>"
           "http-get:*:audio/mpeg:*,http-get:*:audio/mp3:*,http-get:*:audio/aac:*,"
           "http-get:*:audio/x-aac:*,http-get:*:audio/flac:*,http-get:*:audio/x-flac:*,"
           "http-get:*:audio/wav:*,http-get:*:audio/x-wav:*,http-get:*:audio/L16:*"
           "</Sink>";
  } else if (b.indexOf("GetCurrentConnectionInfo") != -1) {
    action = "GetCurrentConnectionInfo";
    body = "<RcsID>0</RcsID><AVTransportID>0</AVTransportID>"
           "<ProtocolInfo>http-get:*:audio/mpeg:*</ProtocolInfo>"
           "<PeerConnectionManager></PeerConnectionManager><PeerConnectionID>-1</PeerConnectionID>"
           "<Direction>Output</Direction><Status>OK</Status>";
  } else if (b.indexOf("GetCurrentConnectionIDs") != -1) {
    action = "GetCurrentConnectionIDs";
    body = "<ConnectionIDs>0</ConnectionIDs>";
  } else {
    action = "GetCurrentConnectionIDs";   // 兜底给合法响应
    body = "<ConnectionIDs></ConnectionIDs>";
  }
  String resp = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + action + "Response xmlns:u=\"" + ns + "\">" + body + "</u:" + action + "Response></s:Body></s:Envelope>";
  m_server.send(200, "text/xml; charset=utf-8", resp);
  Serial.printf("[CM] %s\n", action.c_str());
}

// ============ RenderingControl SOAP（音量 0-100 ↔ Source 0-21） ============
void ControlPanel::handleRC() {
  String b = m_server.arg("plain");
  String ns = "urn:schemas-upnp-org:service:RenderingControl:1";
  String action, body;
  if (b.indexOf("SetVolume") != -1) {
    action = "SetVolume";
    int v = m_volPct;
    int p = b.indexOf("<DesiredVolume>");
    if (p != -1) {
      int e = b.indexOf("<", p + 15);
      if (e != -1) v = b.substring(p + 15, e).toInt();
    }
    m_volPct = constrain(v, 0, 100);
    if (m_src) m_src->setVolume(m_volPct * 21 / 100);
    Serial.printf("[RC] 音量=%d%%\n", m_volPct);
  } else if (b.indexOf("GetMute") != -1) {
    action = "GetMute";
    body = "<CurrentMute>0</CurrentMute>";
  } else {   // GetVolume / 其它
    action = "GetVolume";
    body = "<CurrentVolume>" + String(m_volPct) + "</CurrentVolume>";
  }
  String resp = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + action + "Response xmlns:u=\"" + ns + "\">" + body + "</u:" + action + "Response></s:Body></s:Envelope>";
  m_server.send(200, "text/xml; charset=utf-8", resp);
}

// ============ GENA 事件订阅 ============
// 应答 SUBSCRIBE/UNSUBSCRIBE（eventSubURL）。状态变化无需真发 NOTIFY——
// 只要订阅成功，控制点就会认为"可投屏"，随后 POST SetAVTransportURI。
void ControlPanel::handleEvent() {
  if (m_server.method() == HTTP_SUBSCRIBE) {
    // 固定 SID；续订请求带原 SID 也应答同值（保持订阅不中断）
    String sid = m_server.header("SID");
    if (!sid.length()) sid = "uuid:9F4A6A12-2C6B-4B2E-9E8C-8C4A0E0A6C21";
    m_server.sendHeader("SID", sid);
    m_server.sendHeader("TIMEOUT", "Second-1800");
    m_server.send(200, "text/plain", "");
    Serial.printf("[GENA] SUBSCRIBE %s\n", m_server.uri().c_str());
  } else {   // UNSUBSCRIBE
    m_server.send(200, "text/plain", "");
    Serial.printf("[GENA] UNSUBSCRIBE %s\n", m_server.uri().c_str());
  }
}

// ============ UPnP/SSDP ============
static const char UDN[] = "uuid:11223344-5566-7788-9900-AABBCCDDEEFF";
static const char DEV_MR[] = "urn:schemas-upnp-org:device:MediaRenderer:1";

void ControlPanel::sendNotify() {
  if (WiFi.status() != WL_CONNECTED) return;
  IPAddress mcast(239, 255, 255, 250);
  String h = "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nCACHE-CONTROL: max-age=180\r\n";
  h += "LOCATION: http://" + WiFi.localIP().toString() + "/desc.xml\r\n";
  h += "SERVER: ESP32/1.0 UPnP/1.0 ESP32_Multiroom/1.0\r\nNTS: ssdp:alive\r\n";
  // 两个 alive：rootdevice + MediaRenderer（USN 带类型后缀，控制点按此归类）
  String n = h + "NT: upnp:rootdevice\r\nUSN: " + String(UDN) + "::upnp:rootdevice\r\n\r\n";
  m_udpUpnp.beginPacket(mcast, 1900);
  m_udpUpnp.print(n);
  m_udpUpnp.endPacket();
  n = h + "NT: " + String(DEV_MR) + "\r\nUSN: " + String(UDN) + "::" + DEV_MR + "\r\n\r\n";
  m_udpUpnp.beginPacket(mcast, 1900);
  m_udpUpnp.print(n);
  m_udpUpnp.endPacket();
}

// 发单条单播 SSDP 响应（M-SEARCH 的应答必须是单播到来源端口）
void ControlPanel::sendSearchResp(const IPAddress& ip, uint16_t port,
                                  const String& st, const String& usn) {
  if (WiFi.status() != WL_CONNECTED) return;
  String r = "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=180\r\nEXT:\r\n";
  r += "LOCATION: http://" + WiFi.localIP().toString() + "/desc.xml\r\n";
  r += "SERVER: ESP32/1.0 UPnP/1.0 ESP32_Multiroom/1.0\r\n";
  r += "ST: " + st + "\r\nUSN: " + usn + "\r\n\r\n";
  if (!m_udpUpnp.beginPacket(ip, port)) { Serial.println("[SSDP] beginPacket 失败"); return; }
  m_udpUpnp.print(r);
  if (m_udpUpnp.endPacket() == 0)
    Serial.println("[SSDP] endPacket 失败(ENOMEM?)");
  else if ((++s_ssdpResp % 20) == 0)
    Serial.printf("[SSDP] 已响应累计 x%u\n", s_ssdpResp);
}

void ControlPanel::handleSearch() {
  int sz = m_udpUpnp.parsePacket();
  if (!sz) return;
  char buf[600]; memset(buf, 0, 600);
  m_udpUpnp.read(buf, min(sz, 599));
  String req(buf);
  if (req.indexOf("M-SEARCH") == -1) return;

  IPAddress fromIP = m_udpUpnp.remoteIP();
  uint16_t fromPort = m_udpUpnp.remotePort();

  // 提取请求 ST（行首，跳过 "\r\nST:" 共 5 字符）
  String st = "ssdp:all";
  int stPos = req.indexOf("\r\nST:");
  if (stPos == -1) stPos = req.indexOf("\nST:");
  if (stPos != -1) {
    String s = req.substring(stPos + 5);
    s.trim();
    int nl = s.indexOf('\r');
    if (nl == -1) nl = s.indexOf('\n');
    if (nl != -1) s = s.substring(0, nl);
    s.trim();
    if (s.length() > 0) st = s;
  }

  // 按请求类型回一条或多条、ST 与 USN 后缀严格对应的响应（UPnP 规范）
  if (st == "ssdp:all") {
    sendSearchResp(fromIP, fromPort, "upnp:rootdevice", String(UDN) + "::upnp:rootdevice");
    sendSearchResp(fromIP, fromPort, DEV_MR, String(UDN) + "::" + DEV_MR);
    sendSearchResp(fromIP, fromPort, UDN, UDN);
  } else if (st == "upnp:rootdevice") {
    sendSearchResp(fromIP, fromPort, "upnp:rootdevice", String(UDN) + "::upnp:rootdevice");
  } else if (st == DEV_MR || st.indexOf("MediaRenderer") != -1) {
    sendSearchResp(fromIP, fromPort, DEV_MR, String(UDN) + "::" + DEV_MR);
  } else if (st == UDN || st.indexOf("uuid:11223344") != -1) {
    sendSearchResp(fromIP, fromPort, UDN, UDN);
  }
}
