// ============================================================
// main.cpp — DLNA 主节点（应用层）
// 架构：Source(URL解码) → 下混单声道 → Distribute(UDP单播) → 从机
// 分层: source + distribute 库, 本文件只做组装和 Web/UPnP 控制
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiUDP.h>
#include <ESPmDNS.h>
#include <esp32-hal-psram.h>
#include "config.h"             // WiFi/端口（真实值，gitignore）
#include "source_url.h"         // 音源层：URL → PCM
#include "distribute_udp.h"     // 分发层：PCM → UDP 单播

// 主机 I2S 引脚（主机不接 DAC，仅解码库内部需要占位）
#define I2S_BCK  21
#define I2S_WS   19
#define I2S_DATA 20

// 固定音源：开机自动播放（http 直链，https 会让解码库崩溃）
#define STREAM_URL "http://ice1.somafm.com/groovesalad-128-mp3"

WebServer server(80);
WiFiUDP udpUpnp;   // UPnP 发现用

// ---- 分层对象 ----
SourceURL* g_source = NULL;          // 音源（全局指针，供回调转发）
Distribute* g_dist = NULL;           // 分发

// ============ 单声道下混 + 分发（Source PCM 回调） ============
// 立体声 → (L+R)/4 留 6dB headroom → 单声道 int16 → 分发
static int16_t g_mono[2048];
static uint32_t g_pcmCount = 0;

void onSourcePCM(const int16_t* pcm, uint16_t frames) {
  if (!g_dist) return;
  int ch = g_source ? g_source->channels() : 2;
  const int16_t* send = pcm;
  uint16_t n = frames;
  if (ch != 1) {
    if (n > 2048) n = 2048;
    for (uint16_t i = 0; i < n; i++)
      g_mono[i] = (int16_t)(((int32_t)pcm[2*i] + pcm[2*i+1]) >> 2);
    send = g_mono;
  }
  g_pcmCount++;
  g_dist->sendPCM(send, n);
  if ((g_pcmCount % 100) == 0)
    Serial.printf("PCM 分发: frame=%u\n", (unsigned)n);
}

// ============ UPnP/DLNA ============
const char deviceDesc[] PROGMEM = R"XML(
<root xmlns="urn:schemas-upnp-org:device-1-0">
  <specVersion><major>1</major><minor>0</minor></specVersion>
  <device>
    <deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>
    <friendlyName>ESP32 多房间音响</friendlyName>
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
    </serviceList>
  </device>
</root>
)XML";

void sendUPnPNotify() {
  String n = "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nCACHE-CONTROL: max-age=180\r\n";
  n += "LOCATION: http://" + WiFi.localIP().toString() + "/desc.xml\r\n";
  n += "NT: urn:schemas-upnp-org:device:MediaRenderer:1\r\nNTS: ssdp:alive\r\n";
  n += "USN: uuid:11223344-5566-7788-9900-AABBCCDDEEFF\r\n\r\n";
  udpUpnp.beginPacket(IPAddress(239,255,255,250), 1900);
  udpUpnp.print(n); udpUpnp.endPacket();
}

void handleUPnPSearch() {
  int sz = udpUpnp.parsePacket(); if (!sz) return;
  char buf[512]; memset(buf,0,512);
  udpUpnp.read(buf, min(sz,511));
  if (String(buf).indexOf("M-SEARCH") != -1) {
    String r = "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=180\r\nEXT:\r\n";
    r += "LOCATION: http://" + WiFi.localIP().toString() + "/desc.xml\r\n";
    r += "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n";
    r += "USN: uuid:11223344-5566-7788-9900-AABBCCDDEEFF\r\n\r\n";
    udpUpnp.beginPacket(udpUpnp.remoteIP(), udpUpnp.remotePort());
    udpUpnp.print(r); udpUpnp.endPacket();
  }
}

// ============ Web 控制 ============
void handleRoot() {
  String s = (g_source && g_source->isRunning()) ? "▶ 播放中" : "⏹ 已停止";
  String h = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width'><title>ESP32</title><style>body{font-family:sans-serif;padding:20px}input{width:100%;padding:8px;margin:8px 0;box-sizing:border-box}button{padding:10px 20px;margin:4px;font-size:16px}.st{padding:12px;background:#eee;border-radius:4px}</style></head><body>";
  h += "<h1>ESP32 音响</h1><div class='st'>" + s + "</div>";
  h += "<form action='/play' method='POST'><input type='text' name='url' placeholder='音乐URL' required><button>▶ 播放</button></form>";
  h += "<form action='/stop' method='POST'><button>⏹ 停止</button></form>";
  if (g_dist) h += "<p><small>从机数: " + String(g_dist->slaveCount()) + "</small></p>";
  h += "<p><small>IP: " + WiFi.localIP().toString() + "</small></p></body></html>";
  server.send(200, "text/html; charset=utf-8", h);
}

void handlePlay() {
  String url = server.arg("url");
  if (!url.length()) { server.send(400, "text/plain", "no url"); return; }
  Serial.println("播放: " + url);
  if (g_source) g_source->play(url.c_str());
  server.sendHeader("Location", "/"); server.send(302, "text/plain", "OK");
}

void handleStop() {
  if (g_source) g_source->stop();
  server.sendHeader("Location","/"); server.send(302,"text/plain","OK");
}
void handleDesc() { server.send(200,"text/xml; charset=utf-8", deviceDesc); }
void handleSCPD() { server.send(200,"text/xml; charset=utf-8","<?xml version=\"1.0\"?><scpd xmlns=\"urn:schemas-upnp-org:service-1-0\"><actionList><action><name>SetAVTransportURI</name></action></actionList></scpd>"); }

void handleAVTransport() {
  String b = server.arg("plain");
  if (b.indexOf("SetAVTransportURI") != -1) {
    int s = b.indexOf("http://"); if (s != -1) {
      int e = b.indexOf("<", s);
      if (g_source) g_source->play(b.substring(s,e).c_str());
    }
  }
  if (b.indexOf("Pause") != -1) { /* audio.pauseResume 预留 */ }
  if (b.indexOf("Stop") != -1) { if (g_source) g_source->stop(); }
  server.send(200,"text/xml; charset=utf-8","<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body><u:SetAVTransportURIResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\"></u:SetAVTransportURIResponse></s:Body></s:Envelope>");
}

// ============ PSRAM 自检 ============
void psramSelfTest() {
#ifdef BOARD_HAS_PSRAM
  Serial.println("PSRAM 编译开关: ON (BOARD_HAS_PSRAM)");
#else
  Serial.println("PSRAM 编译开关: OFF (未设 BOARD_HAS_PSRAM)");
#endif
  bool ok = psramInit();
  Serial.printf("PSRAM init: %s  total=%uB free=%uB\n",
                ok ? "OK" : "FAIL",
                (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
  if (ok) {
    uint8_t* p = (uint8_t*)ps_malloc(2048);
    if (p) {
      p[0] = 0x55; p[2047] = 0xAA;
      Serial.printf("PSRAM alloc 2KB: OK (%02X..%02X)\n", p[0], p[2047]);
      free(p);
    } else Serial.println("PSRAM alloc 2KB: FAIL");
  }
}

// ============ Setup/Loop ============
void setup() {
  Serial.begin(115200); delay(1000);
  Serial.println("\n===== DLNA 主节点 (模块化) =====");
  psramSelfTest();
  delay(2000);

  WiFi.begin(WIFI_SSID, WIFI_PWD);
  for (int i=0; i<40 && WiFi.status()!=WL_CONNECTED; i++) { delay(500); Serial.print("."); }
  if (WiFi.status() != WL_CONNECTED) { Serial.println("\nWiFi 失败"); return; }
  Serial.println("\nIP: " + WiFi.localIP().toString());

  if (MDNS.begin("esp32-audio")) { MDNS.addService("http","tcp",80); Serial.println("mDNS: esp32-audio.local"); }

  // ---- 分层初始化 ----
  g_dist = new DistributeUDP(AUDIO_PORT, REG_PORT, MAX_SLAVES);
  g_dist->begin();
  Serial.printf("分发层: UDP 单播 (audio=%d reg=%d, max=%d)\n", AUDIO_PORT, REG_PORT, MAX_SLAVES);

  g_source = new SourceURL(I2S_BCK, I2S_WS, I2S_DATA);
  g_source->setCallback(onSourcePCM);
  g_source->setVolume(10);   // 下混已留 6dB 余量
  Serial.println("音源层: URL 解码");

  server.on("/",handleRoot); server.on("/play",HTTP_POST,handlePlay);
  server.on("/stop",HTTP_POST,handleStop); server.on("/desc.xml",handleDesc);
  server.on("/AVTransport/scpd.xml",handleSCPD); server.on("/AVTransport/Control",HTTP_POST,handleAVTransport);
  server.begin(); Serial.println("网页: http://" + WiFi.localIP().toString() + "/");

  udpUpnp.begin(1900); sendUPnPNotify();
  Serial.println("==========================");

  g_source->play(STREAM_URL);
  Serial.print("自动播放: "); Serial.println(STREAM_URL);
}

void loop() {
  static uint32_t tick = 0;
  server.handleClient();
  handleUPnPSearch();
  if (g_dist) g_dist->loop();     // 接收从机 HELLO
  if (g_source) g_source->loop(); // 解码泵
  if (millis()-tick > 30000) { sendUPnPNotify(); tick = millis(); }
}
