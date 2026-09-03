// ============================================================
// control.cpp — 控制面实现（Web + UPnP/DLNA + 自动播放）
// ============================================================
#include "control.h"
#include <ESPmDNS.h>

ControlPanel* g_self = NULL;   // WebServer 回调需要全局指针

const char g_descTmpl[] PROGMEM = R"XML(
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
  m_server.on("/desc.xml", [this]() { handleDesc(); });
  m_server.on("/AVTransport/scpd.xml", [this]() { handleSCPD(); });
  m_server.on("/AVTransport/Control", HTTP_POST, [this]() { handleAVTransport(); });
  m_server.begin();
  Serial.printf("控制面: 网页 http://%s/\n", WiFi.localIP().toString().c_str());

  m_udpUpnp.begin(1900);
  sendNotify();
}

void ControlPanel::loop() {
  m_server.handleClient();
  handleSearch();
  autoPlay();
  if (millis() - m_lastNotify > 30000) {
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
void ControlPanel::handleRoot() {
  String s = (m_src && m_src->isRunning()) ? "▶ 播放中" : "⏹ 已停止";
  String h = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width'><title>ESP32</title><style>body{font-family:sans-serif;padding:20px}input{width:100%;padding:8px;margin:8px 0;box-sizing:border-box}button{padding:10px 20px;margin:4px;font-size:16px}.st{padding:12px;background:#eee;border-radius:4px}</style></head><body>";
  h += "<h1>ESP32 音响</h1><div class='st'>" + s + "</div>";
  h += "<form action='/play' method='POST'><input type='text' name='url' placeholder='音乐URL' required><button>▶ 播放</button></form>";
  h += "<form action='/stop' method='POST'><button>⏹ 停止</button></form>";
  if (m_dist) h += "<p><small>从机数: " + String(m_dist->slaveCount()) + "</small></p>";
  h += "<p><small>IP: " + WiFi.localIP().toString() + "</small></p></body></html>";
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

void ControlPanel::handleDesc() {
  char buf[600];
  snprintf(buf, sizeof(buf), g_descTmpl, m_name.c_str());
  m_server.send(200, "text/xml; charset=utf-8", buf);
}

void ControlPanel::handleSCPD() {
  m_server.send(200, "text/xml; charset=utf-8",
    "<?xml version=\"1.0\"?><scpd xmlns=\"urn:schemas-upnp-org:service-1-0\"><actionList><action><name>SetAVTransportURI</name></action></actionList></scpd>");
}

void ControlPanel::handleAVTransport() {
  String b = m_server.arg("plain");
  if (b.indexOf("SetAVTransportURI") != -1) {
    int s = b.indexOf("http://");
    if (s != -1) {
      int e = b.indexOf("<", s);
      if (m_src) m_src->play(b.substring(s, e).c_str());
    }
  }
  if (b.indexOf("Stop") != -1) { if (m_src) m_src->stop(); }
  m_server.send(200, "text/xml; charset=utf-8",
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body><u:SetAVTransportURIResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\"></u:SetAVTransportURIResponse></s:Body></s:Envelope>");
}

// ============ UPnP/SSDP ============
void ControlPanel::sendNotify() {
  if (WiFi.status() != WL_CONNECTED) return;
  String n = "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nCACHE-CONTROL: max-age=180\r\n";
  n += "LOCATION: http://" + WiFi.localIP().toString() + "/desc.xml\r\n";
  n += "NT: urn:schemas-upnp-org:device:MediaRenderer:1\r\nNTS: ssdp:alive\r\n";
  n += "USN: uuid:11223344-5566-7788-9900-AABBCCDDEEFF\r\n\r\n";
  m_udpUpnp.beginPacket(IPAddress(239, 255, 255, 250), 1900);
  m_udpUpnp.print(n);
  m_udpUpnp.endPacket();
}

void ControlPanel::handleSearch() {
  int sz = m_udpUpnp.parsePacket();
  if (!sz) return;
  char buf[512]; memset(buf, 0, 512);
  m_udpUpnp.read(buf, min(sz, 511));
  if (String(buf).indexOf("M-SEARCH") != -1) {
    String r = "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=180\r\nEXT:\r\n";
    r += "LOCATION: http://" + WiFi.localIP().toString() + "/desc.xml\r\n";
    r += "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n";
    r += "USN: uuid:11223344-5566-7788-9900-AABBCCDDEEFF\r\n\r\n";
    m_udpUpnp.beginPacket(m_udpUpnp.remoteIP(), m_udpUpnp.remotePort());
    m_udpUpnp.print(r);
    m_udpUpnp.endPacket();
  }
}
