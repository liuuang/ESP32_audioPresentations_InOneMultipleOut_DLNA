// ============================================================
// main.cpp — DLNA 主节点（应用层，纯组装）
// 分层: source(解码) + distribute(分发) + control(Web/UPnP)
// 本文件: 只做对象创建与接线，所有逻辑在各 lib 内
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp32-hal-psram.h>
#include "config.h"             // WiFi/端口（真实值，gitignore）
#include "source_url.h"         // 音源层：URL → PCM
#include "distribute_udp.h"     // 分发层：PCM → UDP 单播
#include "control.h"            // 控制面：Web + UPnP/DLNA

// 主机 I2S 引脚（主机不接 DAC，仅解码库内部需要占位）
#define I2S_BCK  21
#define I2S_WS   19
#define I2S_DATA 20

// 固定音源：开机自动播放（http 直链，https 会让解码库崩溃）
#define STREAM_URL "http://ice1.somafm.com/groovesalad-128-mp3"
#define DEVICE_NAME "ESP32 多房间音响"

// ---- 分层对象（全局，供回调转发） ----
static Source* g_source = NULL;
static Distribute* g_dist = NULL;
static ControlPanel g_ctrl;

// ============ PCM 回调：下混单声道 → 分发 ============
// 立体声 → (L+R)/4 留 6dB headroom → 单声道 int16 → 分发
static int16_t g_mono[2048];
static uint32_t g_pcmCount = 0;

void onSourcePCM(const int16_t* pcm, uint16_t frames) {
  if (!g_dist) return;
  int ch = g_source ? g_source->channels() : 2;
  const int16_t* send = pcm;
  uint16_t n = frames;
  if (ch != 1) {
    if (n > 2048) n = 2048;
    for (uint16_t i = 0; i < n; i++)
      g_mono[i] = (int16_t)(((int32_t)pcm[2*i] + pcm[2*i+1]) >> 2);
    send = g_mono;
  }
  g_pcmCount++;
  g_dist->sendPCM(send, n);
  if ((g_pcmCount % 1000) == 0)
    Serial.printf("PCM 分发: frame=%u\n", (unsigned)n);
}

// ============ PSRAM 自检 ============
void psramSelfTest() {
#ifdef BOARD_HAS_PSRAM
  Serial.println("PSRAM 编译开关: ON (BOARD_HAS_PSRAM)");
#else
  Serial.println("PSRAM 编译开关: OFF (未设 BOARD_HAS_PSRAM)");
#endif
  bool ok = psramInit();
  Serial.printf("PSRAM init: %s  total=%uB free=%uB\n",
                ok ? "OK" : "FAIL",
                (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
  if (ok) {
    uint8_t* p = (uint8_t*)ps_malloc(2048);
    if (p) {
      p[0] = 0x55; p[2047] = 0xAA;
      Serial.printf("PSRAM alloc 2KB: OK (%02X..%02X)\n", p[0], p[2047]);
      free(p);
    } else Serial.println("PSRAM alloc 2KB: FAIL");
  }
}

// ============ Setup/Loop（纯组装） ============
void setup() {
  Serial.begin(115200); delay(1000);
  Serial.println("\n===== DLNA 主节点 (模块化) =====");
  psramSelfTest();
  delay(2000);

  WiFi.begin(WIFI_SSID, WIFI_PWD);
  for (int i=0; i<40 && WiFi.status()!=WL_CONNECTED; i++) { delay(500); Serial.print("."); }
  if (WiFi.status() != WL_CONNECTED) { Serial.println("\nWiFi 失败"); return; }
  Serial.println("\nIP: " + WiFi.localIP().toString());

  if (MDNS.begin("esp32-audio")) { MDNS.addService("http","tcp",80); Serial.println("mDNS: esp32-audio.local"); }

  // ---- 分层接线 ----
  g_dist = new DistributeUDP(AUDIO_PORT, REG_PORT, CTRL_PORT, MAX_SLAVES);
  g_dist->begin();
  Serial.printf("分发层: UDP (audio=%d reg=%d ctrl=%d max=%d)\n", AUDIO_PORT, REG_PORT, CTRL_PORT, MAX_SLAVES);

  g_source = new SourceURL(I2S_BCK, I2S_WS, I2S_DATA);
  g_source->setCallback(onSourcePCM);
  g_source->setVolume(10);   // 下混已留 6dB 余量
  Serial.println("音源层: URL 解码");

  g_ctrl.attach(g_source, g_dist);
  g_ctrl.begin(DEVICE_NAME);
  g_ctrl.setAutoPlayURL(STREAM_URL);
  Serial.println("==========================");
}

void loop() {
  g_ctrl.loop();              // Web + UPnP + 自动播放
  if (g_dist) g_dist->loop(); // 接收从机 HELLO
  if (g_source) g_source->loop(); // 解码泵
}
