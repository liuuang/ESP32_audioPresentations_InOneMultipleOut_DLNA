// ============================================================
// config.example.h — 配置模板（可上传 GitHub）
// 使用：复制为 config.h 并填入真实值。
// ⚠️ config.h 已被 .gitignore 排除，真实密码不会上传。
// ============================================================
#pragma once

// ===== WiFi（必填：你的路由器 SSID 和密码）=====
#define WIFI_SSID "你的WiFi名字"
#define WIFI_PWD  "你的WiFi密码"

// ===== 网络（一般不用改）=====
#define AUDIO_PORT 12346   // PCM 音频流端口（主机发 / 从机收）
#define REG_PORT   12345   // 从机 HELLO 注册端口（主机监听）

// ===== 从机 I2S 引脚（S3 -> PCM5102）=====
// 改接线时只需改这里；BCK/LCK/DIN/SCK 对应模块 6 针，5V/GND 供电
#define I2S_BCK  21
#define I2S_WS   19
#define I2S_DATA 20
#define I2S_MCK  47
