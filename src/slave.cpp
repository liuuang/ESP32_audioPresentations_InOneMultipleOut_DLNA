// ========================================================
// DLNA 多房间音频 — 从节点固件
// 功能：接收 ESP-NOW 音频包 → 重组 → I2S 输出到 PCM5102
// 硬件：ESP32 + PCM5102（GPIO 26=BCK, 25=LCK, 27=DIN）
// ========================================================

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <driver/i2s.h>

// ===== 配置 =====
#include "wifi_config.h"       // WiFi 信息（用于自动获取信道）
#define FRAME_SAMPLES  882     // 20ms × 44.1kHz = 882 个采样/声道
#define FRAME_BYTES    (FRAME_SAMPLES * 2 * 2)  // 3528 字节
#define BUFFER_DEPTH   3       // 帧缓冲深度
#define I2S_BCK        26
#define I2S_WS         25
#define I2S_DATA       27
// =================

// ----- 帧缓冲 -----
// 注意：ESP-NOW 单包 ≤ 250 字节，所以一帧会拆成多个包
//       从节点收到所有分片后组装成一帧，再喂给 I2S
uint8_t frameBuffer[FRAME_BYTES];
uint16_t frameReceived = 0;    // 已收到多少字节
uint32_t frameSeq = 0;         // 当前组装的帧序号

// 就绪帧队列（写/读指针）
uint8_t readyFrames[BUFFER_DEPTH][FRAME_BYTES];
volatile uint8_t writePtr = 0;
uint8_t readPtr = 0;

// ----- ESP-NOW 接收回调 -----
// 数据包格式：[4字节帧序号] + [2字节本包偏移] + [PCM 数据]
// ESP-NOW 单包最多 250 字节，所以一帧拆成 ~15 个包
void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (len < 6) return;

  // 解析包头
  uint32_t thisSeq;
  uint16_t offset;
  memcpy(&thisSeq, data, 4);
  memcpy(&offset, data + 4, 2);
  uint16_t payloadLen = len - 6;

  // 新帧开始 → 清空组装缓冲
  if (thisSeq != frameSeq) {
    frameSeq = thisSeq;
    frameReceived = 0;
    memset(frameBuffer, 0, FRAME_BYTES);
  }

  // 写入数据到组装缓冲
  if (offset + payloadLen <= FRAME_BYTES) {
    memcpy(frameBuffer + offset, data + 6, payloadLen);
    frameReceived += payloadLen;
  }

  // 一帧收齐 → 送入就绪队列
  if (frameReceived >= FRAME_BYTES) {
    memcpy(readyFrames[writePtr], frameBuffer, FRAME_BYTES);
    writePtr = (writePtr + 1) % BUFFER_DEPTH;
    frameReceived = 0;
  }
}

// ----- I2S 初始化 -----
void initI2S() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 44100,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_IRAM,
    .dma_buf_count = 6,
    .dma_buf_len = 256,
    .use_apll = true,
    .tx_desc_auto_clear = true
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
  Serial.println("I2S 已就绪");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n===== DLNA 从节点启动 =====");

  // 连 WiFi（目的是自动对齐主节点所在信道）
  // 连上后 ESP-NOW 自动使用同一信道
  // 连 WiFi → 自动对齐主节点所在信道
  WiFi.begin(WIFI_SSID, WIFI_PWD);
  for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi OK，信道: " + String(WiFi.channel()));
  } else {
    Serial.println("\nWiFi 连接失败，请确认密码正确");
    Serial.println("ESP-NOW 可能需手动设信道（待后续优化）");
  }

  // I2S
  initI2S();

  // ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW 初始化失败！");
    return;
  }
  esp_now_register_recv_cb(onEspNowRecv);
  Serial.println("等待主节点音频流...");
  Serial.println("===========================");
}

void loop() {
  // 就绪队列有整帧 → 喂给 I2S
  if (writePtr != readPtr) {
    size_t written;
    i2s_write(I2S_NUM_0, readyFrames[readPtr], FRAME_BYTES, &written, portMAX_DELAY);
    readPtr = (readPtr + 1) % BUFFER_DEPTH;
  }
}