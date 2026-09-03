// ============================================================
// i2s_dac.cpp — I2S DAC 实现
// 注意: mck_io_num 字段需在结构体首位 (S3)
// ============================================================
#include "i2s_dac.h"

bool I2SDAC::begin(int bck, int ws, int data, int mck, uint32_t sampleRate) {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = sampleRate,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_STAND_I2S | I2S_COMM_FORMAT_STAND_MSB),
    .intr_alloc_flags = ESP_INTR_FLAG_IRAM,
    .dma_buf_count = 8,
    .dma_buf_len = 600,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };
  i2s_pin_config_t pin = {
    .mck_io_num = (mck >= 0) ? mck : I2S_PIN_NO_CHANGE,
    .bck_io_num = bck,
    .ws_io_num = ws,
    .data_out_num = data,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) return false;
  if (i2s_set_pin(I2S_NUM_0, &pin) != ESP_OK) return false;
  i2s_set_clk(I2S_NUM_0, sampleRate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
  i2s_zero_dma_buffer(I2S_NUM_0);
  m_ok = true;
  return true;
}

size_t I2SDAC::write(const int16_t* stereo, uint32_t frames, uint32_t timeoutMs) {
  if (!m_ok) return 0;
  size_t w = 0;
  i2s_write(I2S_NUM_0, stereo, frames * 4, &w, timeoutMs / portTICK_PERIOD_MS);
  return w;
}

size_t I2SDAC::writeZeros(uint32_t bytes, uint32_t timeoutMs) {
  if (!m_ok) return 0;
  static uint8_t zeros[2048];
  size_t w = 0;
  i2s_write(I2S_NUM_0, zeros, bytes, &w, timeoutMs / portTICK_PERIOD_MS);
  return w;
}
