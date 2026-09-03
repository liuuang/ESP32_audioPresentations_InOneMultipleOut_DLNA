// ============================================================
// i2s_dac.h — I2S DAC 输出封装（S3 → PCM5102）
// 封装 i2s 驱动：init / 写立体声 / 写静音补零
// ============================================================
#pragma once
#include <Arduino.h>
#include <driver/i2s.h>

class I2SDAC {
public:
  // 初始化 I2S TX（16bit 立体声）。mck=-1 表示不输出 MCLK
  bool begin(int bck, int ws, int data, int mck, uint32_t sampleRate = 44100);
  // 写一段交错立体声 int16（frames = 帧数），返回实际写入字节数
  size_t write(const int16_t* stereo, uint32_t frames, uint32_t timeoutMs = 20);
  // 写静音补零（欠载时维持 DMA 连续）
  size_t writeZeros(uint32_t bytes, uint32_t timeoutMs = 10);

private:
  bool m_ok = false;
};
