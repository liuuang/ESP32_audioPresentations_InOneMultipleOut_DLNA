#include <Arduino.h>
#include <driver/i2s.h>
#define I2S_BCK 2
#define I2S_WS 3
#define I2S_DATA 4

void setup() {
  Serial.begin(115200); delay(500);
  Serial.println("I2S 测试 v3 - LeftJustified");
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 44100,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_STAND_I2S | I2S_COMM_FORMAT_STAND_MSB),
    .intr_alloc_flags = ESP_INTR_FLAG_IRAM,
    .dma_buf_count = 3,
    .dma_buf_len = 600,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };
  i2s_pin_config_t pin = {
    .bck_io_num = I2S_BCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_DATA,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  Serial.print("install: "); Serial.println(i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL));
  Serial.print("set_pin: "); Serial.println(i2s_set_pin(I2S_NUM_0, &pin));
  i2s_set_clk(I2S_NUM_0, 44100, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
  Serial.println("发声测试中...");
}

void loop() {
  int16_t buf[200];
  for (int i = 0; i < 200; i++) buf[i] = (i < 100) ? 20000 : -20000;
  size_t w;
  esp_err_t e = i2s_write(I2S_NUM_0, buf, sizeof(buf), &w, 1000);
  Serial.print("write: "); Serial.print(e);
  Serial.print(" bytes="); Serial.println(w);
  delay(5);
}

