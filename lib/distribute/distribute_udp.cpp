// ============================================================
// distribute_udp.cpp — UDP 单播分发实现
// 单声道 PCM 输入 → 拆 1200B 包 → 逐个单播注册从机
// ============================================================
#include "distribute_udp.h"
#include <string.h>

#define PCM_CHUNK 1200   // 每包最大 PCM 字节（UDP 载荷 < 1472 安全）

DistributeUDP::DistributeUDP(uint16_t audioPort, uint16_t regPort, uint16_t ctrlPort, int maxSlaves)
  : m_audioPort(audioPort), m_regPort(regPort), m_ctrlPort(ctrlPort), m_max(maxSlaves) {}

bool DistributeUDP::begin() {
  // 注册监听 socket（非阻塞）
  m_regSock = socket(AF_INET, SOCK_DGRAM, 0);
  if (m_regSock < 0) return false;
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(m_regPort);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bind(m_regSock, (struct sockaddr*)&addr, sizeof(addr));
  int fl = fcntl(m_regSock, F_GETFL, 0);
  fcntl(m_regSock, F_SETFL, fl | O_NONBLOCK);

  // 发送 socket（持久复用，避免回调里频繁建/拆）
  m_txSock = socket(AF_INET, SOCK_DGRAM, 0);
  if (m_txSock < 0) return false;
  int opt = 1;
  setsockopt(m_txSock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

  // 控制通道发送 socket（复用，发给从机 CTRL_PORT）
  m_ctrlSock = socket(AF_INET, SOCK_DGRAM, 0);
  if (m_ctrlSock < 0) return false;
  setsockopt(m_ctrlSock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
  return true;
}

void DistributeUDP::addSlave(uint32_t ip) {
  IPAddress a(ip);
  for (int i = 0; i < m_count; i++)
    if (m_ips[i] == a) return;
  if (m_count >= m_max) return;
  m_ips[m_count++] = a;
  Serial.printf("从机注册 #%d: %s\n", m_count, a.toString().c_str());
}

void DistributeUDP::loop() {
  if (m_regSock < 0) return;
  struct sockaddr_in from;
  socklen_t flen = sizeof(from);
  uint8_t buf[64];
  int n = recvfrom(m_regSock, buf, sizeof(buf), 0, (struct sockaddr*)&from, &flen);
  if (n > 0) addSlave(from.sin_addr.s_addr);
}

void DistributeUDP::sendChunk(const uint8_t* data, size_t len) {
  if (m_txSock < 0) return;
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(m_audioPort);

  if (m_count > 0) {
    for (int i = 0; i < m_count; i++) {
      addr.sin_addr.s_addr = (uint32_t)m_ips[i];
      sendto(m_txSock, data, len, 0, (struct sockaddr*)&addr, sizeof(addr));
    }
  } else {
    // 无注册从机时广播兜底（启动初期）
    addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(m_txSock, data, len, 0, (struct sockaddr*)&addr, sizeof(addr));
  }
}

void DistributeUDP::sendPCM(const int16_t* pcm, uint16_t samples) {
  static uint8_t pkt[PCM_CHUNK + DIST_HEADER_LEN];
  m_seq++;
  // 输入为单声道 int16 PCM；数据字节数 = samples * 2
  uint32_t remain = (uint32_t)samples * 2;
  uint32_t offset = 0;
  const uint8_t* bytes = (const uint8_t*)pcm;
  while (remain > 0) {
    uint32_t chunk = (remain > PCM_CHUNK) ? PCM_CHUNK : remain;
    memset(pkt, 0, DIST_HEADER_LEN);
    memcpy(pkt, &m_seq, 4);
    memcpy(pkt + 4, &offset, 2);
    memcpy(pkt + 6, &chunk, 2);
    memcpy(pkt + DIST_HEADER_LEN, bytes + offset, chunk);
    sendChunk(pkt, chunk + DIST_HEADER_LEN);
    offset += chunk;
    remain -= chunk;
  }
}

bool DistributeUDP::sendCtrl(const IPAddress& ip, const char* msg) {
  if (m_ctrlSock < 0) return false;
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(m_ctrlPort);
  addr.sin_addr.s_addr = (uint32_t)ip;
  int n = sendto(m_ctrlSock, msg, strlen(msg), 0, (struct sockaddr*)&addr, sizeof(addr));
  return n > 0;
}
