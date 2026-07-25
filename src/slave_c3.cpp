#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <driver/i2s.h>
#include "wifi_config.h"

#define I2S_BCK  2
#define I2S_WS   3
#define I2S_DATA 4
#define FRAME_BYTES 3528

WiFiUDP udp;
uint8_t frameBuf[FRAME_BYTES];
uint16_t framePos = 0;
uint32_t currentSeq = 0;

void initI2S() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 44100,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_STAND_I2S | I2S_COMM_FORMAT_STAND_MSB),
    .intr_alloc_flags = ESP_INTR_FLAG_IRAM,
    .dma_buf_count = 3,
    .dma_buf_len = 600,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };
  i2s_pin_config_t pin = {
    .bck_io_num = I2S_BCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_DATA,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin);
  i2s_zero_dma_buffer(I2S_NUM_0);
  Serial.println("I2S 就绪");
}

void setup() {
  Serial.begin(115200); delay(1000);
  Serial.println("\n===== C3 从节点 =====");
  WiFi.begin(WIFI_SSID, WIFI_PWD);
  int r = 0;
  while (WiFi.status() != WL_CONNECTED && r < 30) { delay(500); Serial.print("."); r++; }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi: " + WiFi.localIP().toString());
    WiFi.setSleep(false);
  } else { Serial.println("WiFi 失败"); return; }
  initI2S();
  udp.begin(12346);
  Serial.println("UDP 就绪");
}

void loop() {
  int pkt = udp.parsePacket();
  if (pkt > 0) {
    Serial.print(".");
    if (pkt >= 8) {
      uint8_t buf[1472];
      int len = udp.read(buf, min(pkt, 1472));
      uint32_t seq; uint16_t off, sz;
      memcpy(&seq, buf, 4); memcpy(&off, buf+4, 2); memcpy(&sz, buf+6, 2);
      if (seq != currentSeq) { currentSeq = seq; framePos = 0; }
      if (off + sz <= FRAME_BYTES) { memcpy(frameBuf+off, buf+8, sz); framePos += sz; }
      if (framePos >= FRAME_BYTES) {
        Serial.print("!");
        size_t w;
        i2s_write(I2S_NUM_0, frameBuf, FRAME_BYTES, &w, 100);
        framePos = 0;
      }
    }
  }
}
