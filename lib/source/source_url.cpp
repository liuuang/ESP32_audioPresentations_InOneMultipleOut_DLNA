// ============================================================
// source_url.cpp — URL 音源实现
// ============================================================
#include "source_url.h"
#include "config.h"   // 端口等

// 全局补丁回调（ESP32-audioI2S 库在 playAudioData() 内调用）
// 库 Audio.h 已声明 extern void codex_audio_callback（C++ linkage），
// 此处定义实现并转发到当前 SourceURL 实例
static SourceURL* g_activeSource = NULL;

void codex_audio_callback(int16_t* buff, uint16_t len) {
  if (g_activeSource) g_activeSource->onPCM(buff, len);
}

SourceURL::SourceURL(int bck, int ws, int data)
  : m_bck(bck), m_ws(ws), m_data(data) {
  g_activeSource = this;
}

bool SourceURL::play(const char* url) {
  if (!m_audioStarted) {
    m_audio.setPinout(m_bck, m_ws, m_data);
    // PSRAM 流缓冲 2MB（压缩音频环形缓冲，仅首次设置）
    m_audio.setBufsize(0, 2 * 1024 * 1024);
    m_audioStarted = true;
  }
  m_audio.connecttohost(url);
  return true;   // connecttohost 返回 void，异步连接；真正成败由 isRunning 判定
}

void SourceURL::onPCM(const int16_t* pcm, uint16_t samples) {
  if (m_cb) m_cb(pcm, samples);
}
