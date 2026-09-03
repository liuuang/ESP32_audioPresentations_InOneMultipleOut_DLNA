// ============================================================
// source.h — 音源层统一接口（主机侧）
// 职责：把"任意来源的音频"变成 PCM 流，经回调交给分发层
// 实现：SourceURL（现在）/ SourceBT（蓝牙，预留）
// ============================================================
#pragma once
#include <Arduino.h>

// PCM 输出回调：从机单声道方案，回调里必须自己下混
// pcm: 交错立体声 int16（或单声道取决于实现），samples: 帧数
typedef void (*SourcePCMCallback)(const int16_t* pcm, uint16_t frames);

class Source {
public:
  virtual ~Source() {}
  // 设置 PCM 回调（由主机分发层消费）
  virtual void setCallback(SourcePCMCallback cb) = 0;
  // 播放 URL / 停止
  virtual bool play(const char* url) = 0;
  virtual void stop() = 0;
  // 每周期调用（解码泵）
  virtual void loop() = 0;
  // 状态
  virtual bool isRunning() = 0;
  // 音量 0-21
  virtual void setVolume(int v) = 0;
  // 声道数（1=单声道 2=立体声）
  virtual int channels() = 0;
};
