// ============================================================
// test_usb_speaker.cpp — 从机 USB Host 驱动 USB 音箱实验
// 验证: ESP32-S3 的 OTG 口(host 模式)能否驱动小爱音箱Pro/USB声卡出声
// 用法: OTG 口插 USB 音箱/声卡(如小爱Pro接电脑那根线)
// 若出声: 440Hz+880Hz 双音 = USB host 音频通路打通
// ============================================================
#include <Arduino.h>
#include <EspUsbHost.h>

EspUsbHost usb;

static constexpr uint32_t TONE_HZ_LEFT = 440;
static constexpr uint32_t TONE_HZ_RIGHT = 880;
static constexpr int16_t VOLUME = 2000;   // 0-32767，避免太响

static uint8_t audioAddress = 0;
static uint32_t s_underrun = 0;

// 接受设备支持的格式（很多 USB 音箱固定 48k/2ch/16bit）
static bool isSupportedOutputStream(uint32_t sampleRate, uint8_t channels, uint8_t bitsPerSample) {
  return bitsPerSample == 16;   // 16bit 即可，采样率/声道跟随设备
}

static void fillTone(EspUsbHostAudioOutputRequest &request) {
  static uint32_t phaseL = 0, phaseR = 0;
  for (size_t frame = 0; frame < request.frameCount; frame++) {
    int16_t vL = (phaseL < request.sampleRate / 2) ? VOLUME : -VOLUME;
    int16_t vR = (phaseR < request.sampleRate / 2) ? VOLUME : -VOLUME;
    phaseL += TONE_HZ_LEFT;
    phaseR += TONE_HZ_RIGHT;
    if (phaseL >= request.sampleRate) phaseL -= request.sampleRate;
    if (phaseR >= request.sampleRate) phaseR -= request.sampleRate;
    for (uint8_t ch = 0; ch < request.channels; ch++) {
      int16_t v = (ch == 0) ? vL : vR;
      size_t off = (frame * request.channels + ch) * request.bytesPerSample;
      request.data[off] = v & 0xff;
      request.data[off + 1] = (v >> 8) & 0xff;
    }
  }
  request.writtenFrames = request.frameCount;
  if (request.frameCount == 0) s_underrun++;
}

void setup() {
  Serial.begin(115200);
  delay(3000);
  Serial.println("\n===== USB Host 音箱测试 =====");
  Serial.println("把 USB 音箱/声卡插到 OTG 口");

  usb.onDeviceConnected([](const EspUsbHostDeviceInfo &info) {
    Serial.print("设备连接: "); espUsbHostPrint(info);
    if (usb.audioOutputReady(info.address)) {
      EspUsbHostAudioStreamInfo streams[4];
      size_t cnt = usb.getAudioStreams(info.address, streams, 4);
      for (size_t i = 0; i < cnt; i++) {
        Serial.printf(" 流 %u: %uHz %uch %ubit\n", (unsigned)i,
                      streams[i].sampleRate, streams[i].channels, streams[i].bitsPerSample);
      }
      auto sel = espUsbHostSelectAudioOutputStream(streams, cnt, isSupportedOutputStream);
      if (sel) {
        if (usb.audioOutputStart(streams[sel.index], sel.sampleRate, info.address)) {
          audioAddress = info.address;
          Serial.printf("✅ 音频输出已启动: addr=%u rate=%u\n",
                        info.address, streams[sel.index].sampleRate);
        } else {
          Serial.printf("audioOutputStart 失败: %s\n", usb.lastErrorName());
        }
      } else {
        Serial.println("❌ 无匹配音频流（设备不支持16bit?）");
      }
    } else {
      Serial.println("设备不是 USB 音频输出设备");
    }
  });

  usb.onDeviceDisconnected([](const EspUsbHostDeviceInfo &info) {
    Serial.print("设备断开: "); espUsbHostPrint(info);
    if (info.address == audioAddress) audioAddress = 0;
  });

  usb.onAudioOutputRequest([](EspUsbHostAudioOutputRequest &request) {
    fillTone(request);
  });

  if (!usb.begin()) {
    Serial.printf("usb.begin 失败: %s\n", usb.lastErrorName());
  }
}

void loop() {
  static uint32_t last = 0;
  if (audioAddress != 0 && millis() - last > 1000) {
    last = millis();
    Serial.printf("播放中 addr=%u underruns=%u\n", audioAddress, (unsigned)s_underrun);
  }
  delay(10);
}
