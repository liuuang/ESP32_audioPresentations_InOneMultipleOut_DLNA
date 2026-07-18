 // ========================================================
 // DLNA 多房间音频 — 从节点固件
 // 功能：接收 ESP-NOW 音频帧 → I2S 输出到 PCM5102
 // 硬件：ESP32 + PCM5102（GPIO 26=BCK, 25=LCK, 27=DIN）
 // ========================================================
 
 #include <Arduino.h>
 #include <WiFi.h>
 #include <esp_now.h>
 #include <driver/i2s.h>
 
 // ===== 必须和主节点完全一致 =====
 #define FRAME_BYTES   3528   // 44.1kHz × 20ms × 2ch × 2bytes
 #define BUFFER_DEPTH  3      // 环形缓冲深度
 #define WIFI_CHANNEL  1      // 改成你路由器的 2.4G 信道
 
 #define I2S_BCK       26
 #define I2S_WS        25
 #define I2S_DATA      27
 // ================================
 
 // ----- 环形缓冲区 -----
 // 写指针在主节点发数据时被回调更新（中断中）
 // 读指针在 loop() 中由 I2S 输出消耗
 uint8_t ringBuffer[BUFFER_DEPTH][FRAME_BYTES];
 volatile uint8_t writePtr = 0;
 uint8_t readPtr = 0;
 uint32_t lastFrameSeq = 0;
 
 // ----- ESP-NOW 接收回调（中断安全）-----
 void onEspNowRecv(const uint8_t *macAddr, const uint8_t *data, int dataLen) {
   if (dataLen < 4) return;
   uint32_t currentSeq;
   memcpy(&currentSeq, data, 4);
   if (currentSeq <= lastFrameSeq) return;  // 丢弃过期帧
   lastFrameSeq = currentSeq;
   memcpy(ringBuffer[writePtr], data + 4, dataLen - 4);
   writePtr = (writePtr + 1) % BUFFER_DEPTH;
 }
 
 // ----- 配置 I2S 音频输出 -----
 void initI2S() {
   i2s_config_t i2sConfig = {
     .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
     .sample_rate = 44100,
     .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
     .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
     .communication_format = I2S_COMM_FORMAT_STAND_I2S,
     .intr_alloc_flags = ESP_INTR_FLAG_IRAM,
     .dma_buf_count = 6,
     .dma_buf_len = 256,
     .use_apll = true,
     .tx_desc_auto_clear = true
   };
   i2s_pin_config_t pinConfig = {
     .bck_io_num = I2S_BCK,
     .ws_io_num = I2S_WS,
     .data_out_num = I2S_DATA,
     .data_in_num = I2S_PIN_NO_CHANGE
   };
   i2s_driver_install(I2S_NUM_0, &i2sConfig, 0, NULL);
   i2s_set_pin(I2S_NUM_0, &pinConfig);
   i2s_zero_dma_buffer(I2S_NUM_0);
   Serial.println("I2S 已就绪");
 }
 
 void setup() {
   Serial.begin(115200);
   delay(1000);
   Serial.println("\n===== DLNA 从节点启动 =====");
 
   // 固定 WiFi 信道（必须和主节点同一信道才能收到 ESP-NOW 广播）
   WiFi.mode(WIFI_STA);
   WiFi.setChannel(WIFI_CHANNEL);
   Serial.print("信道设为: ");
   Serial.println(WIFI_CHANNEL);
 
   // I2S
   initI2S();
 
   // ESP-NOW
   if (esp_now_init() != ESP_OK) {
     Serial.println("ESP-NOW 初始化失败！");
     return;
   }
   esp_now_register_recv_cb(onEspNowRecv);
   Serial.println("等待主节点音频流...");
   Serial.println("===========================");
 }
 
 void loop() {
   // 缓冲区有数据就喂给 I2S 播放
   if (writePtr != readPtr) {
     size_t bytesWritten;
     i2s_write(I2S_NUM_0, ringBuffer[readPtr], FRAME_BYTES, &bytesWritten, portMAX_DELAY);
     readPtr = (readPtr + 1) % BUFFER_DEPTH;
   }
 }
