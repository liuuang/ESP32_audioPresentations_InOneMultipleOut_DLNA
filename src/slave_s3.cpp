// ============================================================
// slave_s3.cpp — S3 从节点（应用层）
// 架构: UDP 收包 → PCMBuf(PSRAM 环形缓冲) → 单声道扩立体声 → I2SDAC
// 分层: pcmbuf + i2s_dac 库, 本文件只做组装与 UDP 收包任务
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "config.h"      // WiFi/端口（真实值，gitignore）
#include "pcmbuf.h"
#include "i2s_dac.h"

// ===== S3 从机 I2S 引脚（S3 -> PCM5102）=====
#define I2S_BCK  21
#define I2S_WS   19
#define I2S_DATA 20
#define I2S_MCK  47     // S3 MCLK 输出 -> 模块 SCK

#define RING_BYTES (2*1024*1024)   // PSRAM 环形缓冲 2MB ≈ 23s
#define PLAY_START_THRESHOLD 16384 // 攒够 ~186ms 才开始播

WiFiUDP udp;
PCMBuf g_ring;               // 环形缓冲（PSRAM）
I2SDAC g_dac;                // I2S DAC

volatile uint32_t currentSeq = 0;
volatile uint32_t lastFrameMs = 0;
volatile uint32_t pktCount = 0;
volatile uint32_t frameCount = 0;
volatile uint32_t seqGaps = 0;

// ============ UDP 接收任务（高优先级） ============
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
    // HELLO 频率：失联(>3s无音频)时每秒广播加速重连；正常每 5s 保活
    bool stale = (millis() - lastFrameMs > 3000);
    uint32_t interval = (haveMaster && !stale) ? 5000 : 1000;
    if (millis() - lastHello > interval) {
      lastHello = millis();
      sendHello(IPAddress(255,255,255,255));
      if (haveMaster && stale) Serial.println("[HELLO] 失联加速重连");
    }
    // 内层循环清空 lwIP 缓冲，避免积压丢包
    int pkt = udp.parsePacket();
    while (pkt > 0) {
      pktCount++;
      if (!haveMaster) {   // 首包学到主机 IP，立刻单播回注册
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
        g_ring.push(buf + 8, sz);   // PCM 进 PSRAM 环形缓冲
      }
      pkt = udp.parsePacket();
    }
    vTaskDelay(1);
  }
}

// ============ Setup/Loop ============
void setup() {
  Serial.begin(115200); delay(1000);
  Serial.println("\n===== S3 从节点 v8 (模块化) =====");

  // 环形缓冲 PSRAM 2MB
  if (!g_ring.init(RING_BYTES, true)) {
    Serial.println("PSRAM 环形缓冲分配失败!");
    while(1) delay(1000);
  }
  Serial.printf("环形缓冲: PSRAM %uKB\n", (unsigned)(RING_BYTES/1024));

  // I2S DAC（S3 MCLK=47）
  if (!g_dac.begin(I2S_BCK, I2S_WS, I2S_DATA, I2S_MCK, 44100)) {
    Serial.println("I2S 初始化失败!");
    while(1) delay(1000);
  }
  Serial.println("I2S 就绪 (MCLK=GPIO47)");

  WiFi.begin(WIFI_SSID, WIFI_PWD);
  int r = 0;
  while (WiFi.status() != WL_CONNECTED && r < 30) { delay(500); Serial.print("."); r++; }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi: " + WiFi.localIP().toString());
    WiFi.setSleep(false);
  } else { Serial.println("WiFi 失败"); return; }

  udp.begin(AUDIO_PORT);
  Serial.println("UDP 就绪");

  xTaskCreatePinnedToCore(udpTask, "udpRx", 4096, NULL, 3, NULL, 1);
  Serial.println("UDP 任务已启动");
}

void loop() {
  static bool playing = false;
  static uint32_t playStartMs = 0;
  static uint32_t lastStatMs = 0;
  static uint32_t bytesPlayed = 0;
  static int16_t mSamples[512];    // 单声道临时
  static int16_t stereo[1024];     // 扩展左右：512 帧

  // ---- I2S 消费状态机 ----
  uint32_t avail = g_ring.length();
  if (!playing) {
    if (avail >= PLAY_START_THRESHOLD) {
      playing = true;
      playStartMs = millis();
      Serial.printf(">>> 开始播放 (ring=%uB)\n", (unsigned)avail);
    }
  } else {
    if (avail >= 2) {
      uint32_t n = avail / 2;               // 单声道采样数
      if (n > 512) n = 512;
      g_ring.read(mSamples, n * 2);
      for (uint32_t i = 0; i < n; i++) {    // 单声道 → 左右同值
        int16_t v = mSamples[i];
        stereo[2*i] = v; stereo[2*i+1] = v;
      }
      size_t w = g_dac.write(stereo, n, 20);
      bytesPlayed += w;
      if (w > 0) lastFrameMs = millis();
      // 注: 若 i2s_write 部分写入(慢流), 写不掉的采样直接丢弃——
      // 迟到的数据补播会乱序, 不如丢(人耳对零星丢样不敏感)
    } else {
      g_dac.writeZeros(1024, 10);           // 欠载补零
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
                  (unsigned)bytesPlayed, (unsigned)g_ring.length(),
                  (unsigned)g_ring.lost());
  }
  vTaskDelay(1);
}
