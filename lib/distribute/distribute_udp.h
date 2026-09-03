// ============================================================
// distribute_udp.h — UDP 单播分发实现
// 从机发 HELLO 注册 → 主机记录 IP → sendPCM 逐个单播
// ============================================================
#pragma once
#include "distribute.h"
#include <lwip/sockets.h>

class DistributeUDP : public Distribute {
public:
  DistributeUDP(uint16_t audioPort, uint16_t regPort, uint16_t ctrlPort, int maxSlaves = 15);
  bool begin() override;
  void loop() override;
  void sendPCM(const int16_t* pcm, uint16_t samples) override;
  int slaveCount() override { return m_count; }
  IPAddress slaveIP(int i) override { return (i >= 0 && i < m_count) ? m_ips[i] : IPAddress(); }
  bool sendCtrl(const IPAddress& ip, const char* msg) override;

private:
  uint16_t m_audioPort;
  uint16_t m_regPort;
  uint16_t m_ctrlPort;
  int      m_max;
  IPAddress m_ips[15];
  int      m_count = 0;
  int      m_regSock = -1;
  int      m_txSock = -1;
  int      m_ctrlSock = -1;
  uint32_t m_seq = 0;

  void addSlave(uint32_t ip);
  void sendChunk(const uint8_t* data, size_t len);
};
