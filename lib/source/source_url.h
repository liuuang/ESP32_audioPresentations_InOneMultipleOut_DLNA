// ============================================================
// source_url.h — URL 音源实现（封装 ESP32-audioI2S 解码库）
// 注: 需要 ESP32-audioI2S 库的 codex_audio_callback 补丁
// (库内 playAudioData() 末尾调用该全局函数)
// ============================================================
#pragma once
#include "source.h"
#include <Audio.h>

class SourceURL : public Source {
public:
  SourceURL(int bck, int ws, int data);

  void setCallback(SourcePCMCallback cb) override { m_cb = cb; }
  bool play(const char* url) override;
  void stop() override { m_audio.stopSong(); m_playedFrames = 0; }
  void loop() override { m_audio.loop(); }
  bool isRunning() override { return m_audio.isRunning(); }
  void setVolume(int v) override { m_audio.setVolume(v); }
  int channels() override { return m_audio.getChannels(); }
  // 进度：优先自计时（按已播放帧数累加，与流类型无关、精确）；
  // 库的 getAudioCurrentTime 依赖文件总时长，http 流可能不可用
  uint32_t positionSec() override { return m_playedFrames / 44100; }
  uint32_t durationSec() override { return m_audio.getAudioFileDuration(); }

  // 供库补丁调用（codex_audio_callback 全局函数转发到此）
  void onPCM(const int16_t* pcm, uint16_t samples);
  Audio& audio() { return m_audio; }

private:
  Audio m_audio;
  SourcePCMCallback m_cb = NULL;
  int m_bck, m_ws, m_data;
  bool m_audioStarted = false;
  volatile uint64_t m_playedFrames = 0;   // 已播放立体声帧数（44100帧=1秒）
};
