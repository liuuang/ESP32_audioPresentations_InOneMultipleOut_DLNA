#include <WiFi.h>
#include <WiFiUdp.h>
#include "wifi_config.h"

WiFiUDP udp;

void setup() {
  Serial.begin(115200); delay(1000);
  Serial.println("\n===== UDP 测试(接收) =====");
  WiFi.begin(WIFI_SSID, WIFI_PWD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("IP: " + WiFi.localIP().toString());
  udp.begin(12346);
  Serial.println("等待中...");
}

void loop() {
  int pkt = udp.parsePacket();
  if (pkt) {
    char buf[64];
    int len = udp.read(buf, 63);
    buf[len] = 0;
    Serial.print("收到: ");
    Serial.println(buf);
  }
}