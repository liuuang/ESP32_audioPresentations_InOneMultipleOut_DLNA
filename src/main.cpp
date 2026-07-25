#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiUDP.h>
#include <ESPmDNS.h>
#include <Audio.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include "wifi_config.h"

#define I2S_BCK  26
#define I2S_WS   25
#define I2S_DATA 27

WebServer server(80);
WiFiUDP udpUpnp;  // only for UPnP
Audio audio;

// ============ 底层 UDP 广播（独立于 WiFiUDP 类） ============
void udpBroadcast(const uint8_t* data, size_t len, uint16_t port) {
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) return;
  int opt = 1;
  setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
  sendto(sock, data, len, 0, (struct sockaddr*)&addr, sizeof(addr));
  close(sock);
}

// ============ PCM 回调 ============
void codex_audio_callback(int16_t* buff, uint16_t len) {
  static uint8_t pkt[520];
  static uint32_t seq = 0; seq++;
  uint16_t remain = len * 2;
  uint16_t offset = 0;
  uint8_t* bytes = (uint8_t*)buff;
  while (remain > 0) {
    uint16_t chunk = (remain > 512) ? 512 : remain;
    memset(pkt, 0, 8);
    memcpy(pkt, &seq, 4);
    memcpy(pkt+4, &offset, 2);
    memcpy(pkt+6, &chunk, 2);
    memcpy(pkt+8, bytes+offset, chunk);
    udpBroadcast(pkt, chunk + 8, 12346);
    offset += chunk;
    remain -= chunk;
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
void setup() {
  Serial.begin(115200); delay(1000);
  Serial.println("\n===== DLNA 主节点 =====");

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

  audio.setPinout(I2S_BCK,I2S_WS,I2S_DATA);
  audio.setBufsize(1024*8,1024*8);
  audio.setVolume(15);
  Serial.println("==========================");
}

void loop() {
  static uint32_t tick = 0;
  server.handleClient(); handleUPnPSearch(); audio.loop();
  if (millis()-tick > 30000) { sendUPnPNotify(); tick = millis(); }
}