// ============================================================
// pcmbuf.h — 环形 PCM 抖动缓冲（纯逻辑，无硬件依赖）
// 生产端(网络) push / 消费端(I2S) read，线程安全需外部保证单写单读
// ============================================================
#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

class PCMBuf {
public:
  // size: 缓冲字节数（应为 2 的幂，代码不强制但 % 运算需注意）
  // usePsram: true → ps_malloc；false → 内部 RAM
  bool init(uint32_t size, bool usePsram);
  void deinit();

  // 生产端：写入 n 字节；满则丢弃并计数（返回实际写入字节数）
  uint32_t push(const uint8_t* d, uint32_t n);
  // 消费端：读出最多 len 字节到 dst，返回实际读出字节数
  uint32_t read(void* dst, uint32_t len);
  // 当前水位（字节）
  uint32_t length();
  // 丢弃的字节数（调试用）
  uint32_t lost() { return m_lost; }

  bool ready() { return m_buf != NULL; }

private:
  uint8_t* m_buf = NULL;
  uint32_t m_size = 0;
  volatile uint32_t m_rd = 0;
  volatile uint32_t m_wr = 0;
  volatile uint32_t m_lost = 0;
};
