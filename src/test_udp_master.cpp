#include <WiFi.h>
#include <WiFiUdp.h>
#include "wifi_config.h"

WiFiUDP udp;
uint32_t last = 0;
int cnt = 0;

void setup() {
  Serial.begin(115200); delay(1000);
  Serial.println("\n===== UDP 测试(发送) =====");
  WiFi.begin(WIFI_SSID, WIFI_PWD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nIP: " + WiFi.localIP().toString());
}

void loop() {
  if (millis() - last > 1000) {
    String msg = "HELLO " + String(cnt++);
    udp.beginPacket(IPAddress(255,255,255,255), 12346);
    udp.print(msg);
    udp.endPacket();
    Serial.println("Send: " + msg);
    last = millis();
  }
}