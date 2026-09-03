// ============================================================
// control.h — 控制面层（主机侧）
// 职责：Web 控制页 + UPnP/DLNA 发现 + 自动播放
// 依赖：Source(控制播放) + Distribute(显示从机数)
// 实现：ControlPanel —— main/slave 应用组装时挂载
// ============================================================
#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <WiFiUDP.h>
#include "source.h"
#include "distribute.h"

class ControlPanel {
public:
  // 挂载依赖层（必须在 begin 前调用）
  void attach(Source* src, Distribute* dist) { m_src = src; m_dist = dist; }
  // 启动：注册 HTTP 路由 + UPnP 监听
  void begin(const char* friendlyName);
  // 每周期调用：处理 Web 请求 + UPnP 搜索响应
  void loop();
  // 设置自动播放 URL（WiFi/DNS 就绪后自动连，失败重试）
  void setAutoPlayURL(const char* url);

private:
  Source* m_src = NULL;
  Distribute* m_dist = NULL;
  WebServer m_server{80};
  WiFiUDP m_udpUpnp;        // 保留（网页 UPnP 用不到，暂留）
  int m_ssdpSock = -1;      // SSDP 原生 socket（监听组播+响应）
  String m_name;
  String m_autoURL;
  String m_lastURI;        // 最近一次 SetAVTransportURI 收到的曲目地址（GetMediaInfo 回显）
  int m_volPct = 50;   // RenderingControl 音量 0-100（映射到 Source 的 0-21）
  bool m_autoPlayed = false;
  uint32_t m_lastPlayTry = 0;
  uint32_t m_lastNotify = 0;

  void handleRoot();
  void handlePlay();
  void handleStop();
  void handleDesc();
  void handleSCPD();
  void handleAVTransport();
  void handleCM();
  void handleRC();
  void handleEvent();
  void handleSlaves();      // GET: 从机列表页（音量/延迟滑块）
  void handleSlaveCtl();    // POST: 设置某从机 VOL/DLY
  void sendNotify();
  void sendSearchResp(const IPAddress& ip, uint16_t port, const String& st, const String& usn);
  void handleSearch();
  void autoPlay();
};
