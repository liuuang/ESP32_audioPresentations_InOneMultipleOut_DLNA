// ============================================================
// pcmbuf.cpp — 环形缓冲实现
// 满时丢弃新块（不覆盖未播数据）；只允许单写单读
// ============================================================
#include "pcmbuf.h"

bool PCMBuf::init(uint32_t size, bool usePsram) {
  deinit();
  m_size = size;
  if (usePsram) {
    m_buf = (uint8_t*)ps_malloc(size);
  } else {
    m_buf = (uint8_t*)malloc(size);
  }
  if (!m_buf) { m_size = 0; return false; }
  m_rd = m_wr = m_lost = 0;
  return true;
}

void PCMBuf::deinit() {
  if (m_buf) { free(m_buf); m_buf = NULL; }
  m_size = 0;
}

uint32_t PCMBuf::length() {
  return (m_wr - m_rd + m_size) % m_size;
}

uint32_t PCMBuf::push(const uint8_t* d, uint32_t n) {
  if (!m_buf || n >= m_size) return 0;
  // 满则丢新块
  if (length() + n > m_size - 2048) {
    m_lost += n;
    return 0;
  }
  uint32_t wr = m_wr;
  uint32_t s = (wr + n <= m_size) ? n : (m_size - wr);
  memcpy(m_buf + wr, d, s);
  if (s < n) memcpy(m_buf, d + s, n - s);
  m_wr = (wr + n) % m_size;
  return n;
}

uint32_t PCMBuf::read(void* dst, uint32_t len) {
  if (!m_buf || len == 0) return 0;
  uint32_t avail = length();
  if (len > avail) len = avail;
  if (len == 0) return 0;
  uint32_t s = (m_rd + len <= m_size) ? len : (m_size - m_rd);
  memcpy(dst, m_buf + m_rd, s);
  if (s < len) memcpy((uint8_t*)dst + s, m_buf, len - s);
  m_rd = (m_rd + len) % m_size;
  return len;
}
