#include <Arduino.h>
#define SPEAKER 2  // GPIO2 直接接音箱（通过电容）

void setup() {
  pinMode(SPEAKER, OUTPUT);
  Serial.begin(115200);
  Serial.println("PWM 测试 - 直接驱动音箱");
}

void loop() {
  // 1kHz 方波
  digitalWrite(SPEAKER, HIGH);
  delayMicroseconds(500);
  digitalWrite(SPEAKER, LOW);
  delayMicroseconds(500);
}