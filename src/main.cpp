#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiUDP.h>
#include <ESPmDNS.h>
#include <Audio.h>
#include <esp32-hal-psram.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include "config.h"   // WiFi/端口统一配置（真实值，gitignore 不上传）

// 主机 I2S 引脚（主机不接 DAC，仅库需要占位；值随意）
#define I2S_BCK  21
#define I2S_WS   19
#define I2S_DATA 20

// 固定音源：开机自动播放，无需手动输入 URL
// 用 http 直链（ESP32-audioI2S 对 https 流有崩溃问题）
// 换台：直接改这一行 URL 即可
#define STREAM_URL "http://ice1.somafm.com/groovesalad-128-mp3"

WebServer server(80);
WiFiUDP udpUpnp;  // only for UPnP
Audio audio;

// ============ 从机注册表（广播被 AP 隔离时改单播） ============
#define MAX_SLAVES 15   // 最多 15 从机（7.1.4 十一声道 + 余量）
IPAddress slaveIPs[MAX_SLAVES];
int slaveCount = 0;
int regSock = -1;

void addSlave(uint32_t ip) {
  IPAddress a(ip);
  for (int i = 0; i < slaveCount; i++)
    if (slaveIPs[i] == a) return;  // 已注册
  if (slaveCount >= MAX_SLAVES) return;
  slaveIPs[slaveCount++] = a;
  Serial.printf("从机注册 #%d: %s\n", slaveCount, a.toString().c_str());
}

void initRegSock() {
  regSock = socket(AF_INET, SOCK_DGRAM, 0);
  if (regSock < 0) { Serial.println("reg socket 失败"); return; }
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(REG_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bind(regSock, (struct sockaddr*)&addr, sizeof(addr));
  // 非阻塞：没包时立即返回
  int fl = fcntl(regSock, F_GETFL, 0);
  fcntl(regSock, F_SETFL, fl | O_NONBLOCK);
  Serial.printf("注册监听: port %d\n", REG_PORT);
}

void pollRegistrations() {
  if (regSock < 0) return;
  struct sockaddr_in from;
  socklen_t flen = sizeof(from);
  uint8_t buf[64];
  int n = recvfrom(regSock, buf, sizeof(buf), 0, (struct sockaddr*)&from, &flen);
  if (n > 0) addSlave(from.sin_addr.s_addr);
}

// ============ 底层 UDP 发送：优先单播从机，无从机时广播 ============
// 注意：socket 只在启动时创建一次并复用（音频回调里频繁 socket()/close() 会造成
// 巨大开销导致数据抖动 -> "slow stream, dropouts" -> 从机声音突突）
int txSock = -1;

void initTxSock() {
  txSock = socket(AF_INET, SOCK_DGRAM, 0);
  if (txSock < 0) { Serial.println("tx socket 失败"); return; }
  int opt = 1;
  setsockopt(txSock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
}

void udpBroadcast(const uint8_t* data, size_t len, uint16_t port) {
  if (txSock < 0) return;
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);

  if (slaveCount > 0) {
    for (int i = 0; i < slaveCount; i++) {
      addr.sin_addr.s_addr = (uint32_t)slaveIPs[i];
      sendto(txSock, data, len, 0, (struct sockaddr*)&addr, sizeof(addr));
    }
  } else {
    addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(txSock, data, len, 0, (struct sockaddr*)&addr, sizeof(addr));
  }
}

// ============ PCM 回调 ============
// 每包最大 1200 字节 PCM（原 512 太碎，一帧 2304B 拆 5 包 -> 收发开销大、
// 到达突刺明显；1200B 拆 2 包更平滑）。UDP 载荷 < 1472 安全。
#define PCM_CHUNK 1200
void codex_audio_callback(int16_t* buff, uint16_t len) {
  static uint8_t pkt[PCM_CHUNK + 8];
  static int16_t mono[2048];
  static uint32_t seq = 0; seq++;
  static uint32_t lastLog = 0;
  // 链路统一单声道：立体声源下混 (L+R)/2，再衰减一档留 6dB headroom。
  // 原因：互联网电台响度大（峰值常年近 0dBFS），满幅下混会在后级削波，
  // 低音失真。留余量后数字域永不削，响度靠音箱音量补偿。
  // len 是“帧数”（每声道采样数）；单声道每帧 2 字节。
  uint16_t ch = audio.getChannels();
  const int16_t* send = buff;
  uint16_t n = len;
  if (ch != 1) {                        // 0 或 2：按立体声下混（首帧兜底也算立体声）
    if (n > 2048) n = 2048;             // 立体声帧数不可能超过缓冲一半，防御
    for (uint16_t i = 0; i < n; i++)
      mono[i] = (int16_t)(((int32_t)buff[2*i] + buff[2*i+1]) >> 2);  // /4 = 6dB 余量
    send = mono;
  }                                     // 已是单声道源：原样透传
  uint32_t remain = (uint32_t)n * 2;    // 16bit 单声道
  uint32_t offset = 0;
  uint8_t* bytes = (uint8_t*)send;
  while (remain > 0) {
    uint32_t chunk = (remain > PCM_CHUNK) ? PCM_CHUNK : remain;
    memset(pkt, 0, 8);
    memcpy(pkt, &seq, 4);
    memcpy(pkt+4, &offset, 2);
    memcpy(pkt+6, &chunk, 2);
    memcpy(pkt+8, bytes+offset, chunk);
    udpBroadcast(pkt, chunk + 8, 12346);
    offset += chunk;
    remain -= chunk;
  }
  // 周期日志：确认广播在跑（每 100 帧打印一次）
  if (seq - lastLog >= 100) {
    lastLog = seq;
    Serial.printf("PCM broadcast: seq=%u len=%u\n", seq, len);
  }
}

void audio_info(const char *info) {
  Serial.print("音频: "); Serial.println(info);
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

// ============ Web ============
void handleRoot() {
  String s = audio.isRunning() ? "▶ 播放中" : "⏹ 已停止";
  String h = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width'><title>ESP32</title><style>body{font-family:sans-serif;padding:20px}input{width:100%;padding:8px;margin:8px 0;box-sizing:border-box}button{padding:10px 20px;margin:4px;font-size:16px}.st{padding:12px;background:#eee;border-radius:4px}</style></head><body>";
  h += "<h1>ESP32 音响</h1><div class='st'>" + s + "</div>";
  h += "<form action='/play' method='POST'><input type='text' name='url' placeholder='音乐URL' required><button>▶ 播放</button></form>";
  h += "<form action='/stop' method='POST'><button>⏹ 停止</button></form>";
  h += "<p><small>IP: " + WiFi.localIP().toString() + "</small></p></body></html>";
  server.send(200, "text/html; charset=utf-8", h);
}

void handlePlay() {
  String url = server.arg("url");
  if (!url.length()) { server.send(400, "text/plain", "no url"); return; }
  Serial.println("播放: " + url);
  audio.connecttohost(url.c_str());
  server.sendHeader("Location", "/"); server.send(302, "text/plain", "OK");
}

void handleStop() { audio.stopSong(); server.sendHeader("Location","/"); server.send(302,"text/plain","OK"); }
void handleDesc() { server.send(200,"text/xml; charset=utf-8", deviceDesc); }
void handleSCPD() { server.send(200,"text/xml; charset=utf-8","<?xml version=\"1.0\"?><scpd xmlns=\"urn:schemas-upnp-org:service-1-0\"><actionList><action><name>SetAVTransportURI</name></action></actionList></scpd>"); }

void handleAVTransport() {
  String b = server.arg("plain");
  if (b.indexOf("SetAVTransportURI") != -1) {
    int s = b.indexOf("http://"); if (s != -1) {
      int e = b.indexOf("<", s); audio.connecttohost(b.substring(s,e).c_str());
    }
  }
  if (b.indexOf("Pause") != -1) audio.pauseResume();
  if (b.indexOf("Stop") != -1) audio.stopSong();
  server.send(200,"text/xml; charset=utf-8","<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body><u:SetAVTransportURIResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\"></u:SetAVTransportURIResponse></s:Body></s:Envelope>");
}

// ============ Setup/Loop ============
// 开机 PSRAM 自检：编译是否启用 + 运行时可否申请，串口报告
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
      p[0] = 0x55; p[2047] = 0xAA;   // 写读校验
      Serial.printf("PSRAM alloc 2KB: OK (%02X..%02X)\n", p[0], p[2047]);
      free(p);
    } else {
      Serial.println("PSRAM alloc 2KB: FAIL");
    }
  }
}

void setup() {
  Serial.begin(115200); delay(1000);
  Serial.println("\n===== DLNA 主节点 =====");
  psramSelfTest();   // 开机即报 PSRAM 状态，等 2 秒再往下走，方便看清
  delay(2000);

  WiFi.begin(WIFI_SSID, WIFI_PWD);
  for (int i=0; i<40 && WiFi.status()!=WL_CONNECTED; i++) { delay(500); Serial.print("."); }
  if (WiFi.status() != WL_CONNECTED) { Serial.println("\nWiFi 失败"); return; }
  Serial.println("\nIP: " + WiFi.localIP().toString());

  if (MDNS.begin("esp32-audio")) { MDNS.addService("http","tcp",80); Serial.println("mDNS: esp32-audio.local"); }

  server.on("/",handleRoot); server.on("/play",HTTP_POST,handlePlay);
  server.on("/stop",HTTP_POST,handleStop); server.on("/desc.xml",handleDesc);
  server.on("/AVTransport/scpd.xml",handleSCPD); server.on("/AVTransport/Control",HTTP_POST,handleAVTransport);
  server.begin(); Serial.println("网页: http://" + WiFi.localIP().toString() + "/");

  udpUpnp.begin(1900); sendUPnPNotify();

  initRegSock();  // 监听从机 HELLO 注册
  initTxSock();   // 持久 UDP 发送 socket（避免回调里频繁建/拆）

  audio.setPinout(I2S_BCK,I2S_WS,I2S_DATA);
  // 流缓冲(压缩音频)：第一参数=内部RAM，第二参数=PSRAM。
  // PSRAM 2MB ≈ 120s(128kbps) 缓冲，源站长时间停顿也由主机吸收。
  audio.setBufsize(0, 2*1024*1024);   // RAM 不变(0=默认)，PSRAM 2MB
  audio.setVolume(10);   // 下混已留 6dB 余量，音量可回正常
  Serial.println("==========================");

  // 自动播放固定音源（通过性测试：开机即推流，从机出声/LED 即链路通）
  audio.connecttohost(STREAM_URL);
  Serial.print("自动播放: "); Serial.println(STREAM_URL);
}

void loop() {
  static uint32_t tick = 0;
  server.handleClient(); handleUPnPSearch(); audio.loop();
  pollRegistrations();  // 接收从机 HELLO
  if (millis()-tick > 30000) { sendUPnPNotify(); tick = millis(); }
}