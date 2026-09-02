#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <driver/i2s.h>
#include "config.h"   // WiFi/端口/引脚统一配置（真实值，gitignore 不上传）

#define FRAME_BYTES 3528

// ============ 环形抖动缓冲 ============
#define RING_SIZE 131072         // 128KB。链路是 16bit 单声道(88.2KB/s)，≈1.5s 音频
#define PLAY_START_THRESHOLD 16384 // 攒够 ~186ms 才开始播
static uint8_t ring[RING_SIZE];
static volatile uint32_t ringRd = 0;   // 读位置（I2S 消费）
static volatile uint32_t ringWr = 0;   // 写位置（网络生产）
static volatile uint32_t lostBytes = 0;

uint32_t ringLen() { return (ringWr - ringRd + RING_SIZE) % RING_SIZE; }

void ringPush(const uint8_t* d, uint32_t n) {
  if (n >= RING_SIZE) return;
  uint32_t wr = ringWr;
  // 满则丢整块最旧，避免逐字节死循环
  if (ringLen() + n > RING_SIZE - 2048) {
    lostBytes += n;
    return;   // 宁可丢新块，消费端会补零，不卡死
  }
  uint32_t s = (wr + n <= RING_SIZE) ? n : (RING_SIZE - wr);
  memcpy((void*)(ring + wr), d, s);
  if (s < n) memcpy(ring, d + s, n - s);
  ringWr = (wr + n) % RING_SIZE;
}

// 从环形缓冲读出 len 字节到 dst（处理回绕）。仅消费方单线程调用。
void ringRead(void* dst, uint32_t len) {
  uint32_t s = (ringRd + len <= RING_SIZE) ? len : (RING_SIZE - ringRd);
  memcpy(dst, ring + ringRd, s);
  if (s < len) memcpy((uint8_t*)dst + s, ring, len - s);
  ringRd = (ringRd + len) % RING_SIZE;
}

WiFiUDP udp;
volatile uint32_t currentSeq = 0;
volatile uint32_t lastFrameMs = 0;
volatile uint32_t pktCount = 0;
volatile uint32_t frameCount = 0;
volatile uint32_t seqGaps = 0;   // seq 跳号计数（丢帧检测）

// ============ UDP 接收任务（高优先级，独立于 I2S 消费） ============
static IPAddress masterIP;
static bool haveMaster = false;

void sendHello(IPAddress ip) {
  udp.beginPacket(ip, REG_PORT);
  udp.print("HELLO slave_s3");
  udp.endPacket();
}

void udpTask(void* pv) {
  uint32_t lastHello = 0;
  for (;;) {
    // 未注册前每秒广播 HELLO 加速发现；已注册后每 5s 保活即可
    uint32_t interval = haveMaster ? 5000 : 1000;
    if (millis() - lastHello > interval) {
      lastHello = millis();
      sendHello(IPAddress(255,255,255,255));
    }
    // 内层循环：把 lwIP 缓冲里的包全部读完，避免积压溢出丢包
    // （主机每帧发 2 包几乎同时到达，若只收 1 个就会正好丢一半）
    int pkt = udp.parsePacket();
    while (pkt > 0) {
      pktCount++;
      // 首包即回注册：从音频包源 IP 得知主机地址，单播 HELLO，
      // 让主机立刻从“广播”切到“单播”，消除开机广播期的丢包卡顿
      if (!haveMaster) {
        haveMaster = true;
        masterIP = udp.remoteIP();
        sendHello(masterIP);
      }
      if (pkt >= 8) {
        uint8_t buf[1472];
        int len = udp.read(buf, min(pkt, 1472));
        uint32_t seq; uint16_t off, sz;
        memcpy(&seq, buf, 4); memcpy(&off, buf+4, 2); memcpy(&sz, buf+6, 2);
        if (seq != currentSeq) {
          if (currentSeq != 0 && seq != (uint32_t)(currentSeq + 1)) seqGaps++;
          currentSeq = seq;
          if (off == 0) {
            frameCount++;
            lastFrameMs = millis();
          }
        }
        ringPush(buf + 8, sz);
      }
      pkt = udp.parsePacket();   // 继续读下一个，直到清空
    }
    vTaskDelay(1);   // 缓冲清空后再让出 CPU
  }
}

void initI2S() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 44100,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_STAND_I2S | I2S_COMM_FORMAT_STAND_MSB),
    .intr_alloc_flags = ESP_INTR_FLAG_IRAM,
    .dma_buf_count = 8,
    .dma_buf_len = 600,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };
  i2s_pin_config_t pin = {
    .mck_io_num = I2S_MCK,
    .bck_io_num = I2S_BCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_DATA,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin);
  i2s_set_clk(I2S_NUM_0, 44100, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
  i2s_zero_dma_buffer(I2S_NUM_0);
  Serial.println("I2S 就绪 (MCLK=GPIO47)");
}

void setup() {
  Serial.begin(115200); delay(1000);
  Serial.println("\n===== S3 从节点 v6 (无LED) =====");
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

  // 创建 UDP 接收任务：核心1，优先级 3（高于 Arduino loopTask 的 1）
  xTaskCreatePinnedToCore(udpTask, "udpRx", 4096, NULL, 3, NULL, 1);
  Serial.println("UDP 任务已启动");
}

void loop() {
  static bool playing = false;
  static uint32_t playStartMs = 0;
  static uint32_t lastStatMs = 0;
  static uint32_t bytesPlayed = 0;
  static uint8_t zeros[2048] = {0};
  static int16_t mSamples[512];   // 单声道临时（一次最多 512 采样）
  static int16_t stereo[1024];    // 扩成左右声道：512 帧

  // ---- I2S 消费状态机（低优先级，UDP 收包不被它阻塞） ----
  uint32_t avail = ringLen();
  if (!playing) {
    if (avail >= PLAY_START_THRESHOLD) {
      playing = true;
      playStartMs = millis();
      Serial.printf(">>> 开始播放 (ring=%uB)\n", (unsigned)avail);
    }
  } else {
    if (avail >= 2) {
      uint32_t n = avail / 2;               // 可用单声道采样数
      if (n > 512) n = 512;
      ringRead(mSamples, n * 2);             // 从环形缓冲读走（单声道 PCM）
      for (uint32_t i = 0; i < n; i++) {     // 单声道 → 左右同值喂 I2S
        int16_t v = mSamples[i];
        stereo[2*i] = v; stereo[2*i+1] = v;
      }
      size_t w = 0;
      i2s_write(I2S_NUM_0, stereo, n*4, &w, 20);  // 短超时，快速返回继续收包
      bytesPlayed += w;
      uint32_t done = w / 4;                 // I2S 实际吃掉的帧数
      if (done < n)                          // 没写掉的采样退回环形缓冲
        ringRd = (ringRd - (n - done)*2 + RING_SIZE) % RING_SIZE;
      if (w > 0) lastFrameMs = millis();
    } else {
      // 欠载：写一小段静音补零（短超时，别阻塞收包）
      size_t wz = 0;
      i2s_write(I2S_NUM_0, zeros, 1024, &wz, 10);
    }
    if (millis() - playStartMs > 1500 && millis() - lastFrameMs > 1500) {
      playing = false;
      Serial.println("<<< 断流，重新缓冲");
    }
  }

  // 统计
  if (millis() - lastStatMs > 5000) {
    lastStatMs = millis();
    Serial.printf("\n[STAT] pkt=%u frames=%u gaps=%u played=%uB ring=%uB lost=%uB\n",
                  (unsigned)pktCount, (unsigned)frameCount, (unsigned)seqGaps,
                  (unsigned)bytesPlayed, (unsigned)ringLen(), (unsigned)lostBytes);
  }
  vTaskDelay(1);
}
