// ========================================================
// DLNA 多房间音频 — 主节点固件
// 控制方式：网页 (http://IP/) 或 DLNA
// 硬件：ESP32 + PCM5102（GPIO 26=BCK, 25=LCK, 27=DIN）
// ========================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiUDP.h>
#include <ESPmDNS.h>
#include <Audio.h>

// ===== 用户配置 =====
#include "wifi_config.h"

#define I2S_BCK  26
#define I2S_WS   25
#define I2S_DATA 27
// ====================

WebServer server(80);
WiFiUDP udp;
Audio audio;

// ══════════════════════════════════════════════════════════
//  UPnP/DLNA 协议
// ══════════════════════════════════════════════════════════

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
  String notify = "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n";
  notify += "CACHE-CONTROL: max-age=180\r\n";
  notify += "LOCATION: http://" + WiFi.localIP().toString() + "/desc.xml\r\n";
  notify += "NT: urn:schemas-upnp-org:device:MediaRenderer:1\r\n";
  notify += "NTS: ssdp:alive\r\n";
  notify += "USN: uuid:11223344-5566-7788-9900-AABBCCDDEEFF\r\n\r\n";
  udp.beginPacket(IPAddress(239, 255, 255, 250), 1900);
  udp.print(notify);
  udp.endPacket();
}

void handleUPnPSearch() {
  int packetSize = udp.parsePacket();
  if (packetSize == 0) return;
  char buf[512];
  memset(buf, 0, sizeof(buf));
  udp.read(buf, min(packetSize, 511));
  String req = String(buf);
  if (req.indexOf("M-SEARCH") != -1) {
    String resp = "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=180\r\nEXT:\r\n";
    resp += "LOCATION: http://" + WiFi.localIP().toString() + "/desc.xml\r\n";
    resp += "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n";
    resp += "USN: uuid:11223344-5566-7788-9900-AABBCCDDEEFF\r\n\r\n";
    udp.beginPacket(udp.remoteIP(), udp.remotePort());
    udp.print(resp);
    udp.endPacket();
  }
}

// ══════════════════════════════════════════════════════════
//  Web 控制界面
// ══════════════════════════════════════════════════════════

void handleRoot() {
  String status = audio.isRunning() ? "▶ 播放中" : "⏹ 已停止";
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>ESP32 音响</title><style>";
  html += "body{font-family:sans-serif;padding:20px;max-width:480px;margin:auto}";
  html += "input[type=text]{width:100%;padding:8px;margin:8px 0;box-sizing:border-box}";
  html += "button{padding:10px 20px;margin:4px;font-size:16px}";
  html += ".status{padding:12px;background:#eee;border-radius:4px;margin:12px 0}";
  html += "</style></head><body>";
  html += "<h1>ESP32 多房间音响</h1>";
  html += "<div class='status'>状态: " + status + "</div>";
  html += "<form action='/play' method='POST'>";
  html += "<input type='text' name='url' placeholder='粘贴音乐流 URL' required>";
  html += "<button type='submit'>▶ 播放</button>";
  html += "</form>";
  html += "<form action='/stop' method='POST'><button>⏹ 停止</button></form>";
  html += "<hr><p><small>IP: " + WiFi.localIP().toString() + "</small></p>";
  html += "</body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}

void handlePlay() {
  String url = server.arg("url");
  if (url.length() == 0) {
    server.send(400, "text/plain", "URL 不能为空");
    return;
  }
  Serial.println("网页播放: " + url);
  audio.connecttohost(url.c_str());
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "OK");
}

void handleStop() {
  audio.stopSong();
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "OK");
}

void handleDesc() {
  server.send(200, "text/xml; charset=utf-8", deviceDesc);
}

void handleSCPD() {
  server.send(200, "text/xml; charset=utf-8",
    "<?xml version=\"1.0\"?>"
    "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
    "<actionList><action><name>SetAVTransportURI</name></action></actionList></scpd>");
}

void handleAVTransport() {
  String body = server.arg("plain");
  if (body.indexOf("SetAVTransportURI") != -1) {
    int s = body.indexOf("http://");
    if (s != -1) {
      int e = body.indexOf("<", s);
      String url = body.substring(s, e);
      Serial.println("DLNA: " + url);
      audio.connecttohost(url.c_str());
    }
  }
  if (body.indexOf("Pause") != -1) audio.pauseResume();
  if (body.indexOf("Stop") != -1) audio.stopSong();
  server.send(200, "text/xml; charset=utf-8",
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
    "<s:Body><u:SetAVTransportURIResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
    "</u:SetAVTransportURIResponse></s:Body></s:Envelope>");
}

// ══════════════════════════════════════════════════════════
//  PCM 数据回调（弱函数，库自动调用的）
//  后续在这里加 ESP-NOW 广播分发
// ══════════════════════════════════════════════════════════

void audio_process_extern(int16_t* buff, uint16_t len, bool *continueI2S) {
  // 保持 true = 本地 PCM5102 仍然出声
  // buff = PCM 数据, len = 采样数
}

void audio_info(const char *info) {
  Serial.print("音频: ");
  Serial.println(info);
}

// ══════════════════════════════════════════════════════════
//  setup / loop
// ══════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n===== DLNA 主节点 =====");

  // WiFi
  WiFi.begin(WIFI_SSID, WIFI_PWD);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi 失败"); return;
  }
  Serial.println("\nIP: " + WiFi.localIP().toString());

  // mDNS
  if (MDNS.begin("esp32-audio")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS: http://esp32-audio.local/");
  }

  // HTTP 路由
  server.on("/", handleRoot);
  server.on("/play", HTTP_POST, handlePlay);
  server.on("/stop", HTTP_POST, handleStop);
  server.on("/desc.xml", handleDesc);
  server.on("/AVTransport/scpd.xml", handleSCPD);
  server.on("/AVTransport/Control", HTTP_POST, handleAVTransport);
  server.begin();
  Serial.println("网页: http://" + WiFi.localIP().toString() + "/");

  // UPnP
  udp.begin(1900);
  sendUPnPNotify();

  // 音频
  audio.setPinout(I2S_BCK, I2S_WS, I2S_DATA);
  audio.setBufsize(1024 * 8, 1024 * 8);
  audio.setVolume(15);

  // 分发暂缺：后续用 audio_process_extern + ESP-NOW
  Serial.println("==========================");
}

void loop() {
  static uint32_t tick = 0;
  server.handleClient();
  handleUPnPSearch();
  audio.loop();
  if (millis() - tick > 30000) { sendUPnPNotify(); tick = millis(); }
}