// ============================================================
// distribute.h — 分发层统一接口（主机侧）
// 职责：把 PCM 流发送到所有注册的从机
// 实现：DistributeUDP（单播，现在）/ DistributeMcast（组播，预留）
// ============================================================
#pragma once
#include <Arduino.h>
#include <IPAddress.h>

// UDP 数据包格式（固定约定，从机解析）
// [4B 帧序号][2B 偏移][2B 长度][PCM 数据]
#define DIST_HEADER_LEN 8

class Distribute {
public:
  virtual ~Distribute() {}

  // 启动：初始化注册监听等
  virtual bool begin() = 0;
  // 每周期调用：接收从机 HELLO 注册
  virtual void loop() = 0;
  // 发送一帧 PCM（内部自行拆包）
  virtual void sendPCM(const int16_t* pcm, uint16_t samples) = 0;
  // 当前已注册从机数
  virtual int slaveCount() = 0;
  // 取第 i 个从机 IP（网页列表/定向控制用）
  virtual IPAddress slaveIP(int i) = 0;
  // 向指定从机发控制消息（音量/延迟）
  virtual bool sendCtrl(const IPAddress& ip, const char* msg) = 0;
  // 从机在线状态（最近心跳距今 <6s 视为在线）；用于网页显示
  virtual bool slaveOnline(int i) = 0;
};
