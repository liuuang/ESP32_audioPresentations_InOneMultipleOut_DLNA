// ============================================================
// distribute_udp.h — UDP 单播分发实现
// 从机发 HELLO 注册 → 主机记录 IP → sendPCM 逐个单播
// ============================================================
#pragma once
#include "distribute.h"
#include <lwip/sockets.h>

class DistributeUDP : public Distribute {
public:
  struct SlaveState {
    IPAddress ip;
    uint32_t lastSeen = 0;   // 最近一次心跳时间（判断在线）
    int vol = 100;           // 从机回报音量 0-100
    uint32_t delayMs = 0;    // 从机回报延迟
    bool playing = false;    // 从机是否在出声
    uint32_t ringKB = 0;     // 从机缓冲水位
  };

  DistributeUDP(uint16_t audioPort, uint16_t regPort, uint16_t ctrlPort, int maxSlaves = 15);
  bool begin() override;
  void loop() override;
  void sendPCM(const int16_t* pcm, uint16_t samples) override;
  int slaveCount() override { return m_count; }
  IPAddress slaveIP(int i) override { return (i >= 0 && i < m_count) ? m_ips[i] : IPAddress(); }
  bool sendCtrl(const IPAddress& ip, const char* msg) override;
  // 从机状态查询（control 网页用）
  const SlaveState* slaveState(int i) { return (i >= 0 && i < m_count) ? &m_states[i] : NULL; }
  // 在线：最近心跳 <6s
  bool slaveOnline(int i) override {
    if (i < 0 || i >= m_count) return false;
    return (millis() - m_states[i].lastSeen) < 6000;
  }

private:
  uint16_t m_audioPort;
  uint16_t m_regPort;
  uint16_t m_ctrlPort;
  int      m_max;
  IPAddress m_ips[15];
  SlaveState m_states[15];
  int      m_count = 0;
  int      m_regSock = -1;
  int      m_txSock = -1;
  int      m_ctrlSock = -1;
  uint32_t m_seq = 0;

  void addSlave(uint32_t ip);
  void handleStatus(const IPAddress& from, const char* data, int len);
  void sendChunk(const uint8_t* data, size_t len);
};
