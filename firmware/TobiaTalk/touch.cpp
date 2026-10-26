#include "touch.h"
#include <Arduino.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "user_config.h"
#include "src/codec_board/codec_init.h"

#define FT6336_ADDR   0x38
#define TP_RST_PIN    7        // touch controller reset (from the board schematic)

static i2c_master_dev_handle_t touch_dev = NULL;

bool touch_init()
{
  // The audio codec owns the I2C bus. If it is not up yet (or was torn down), create it the same way.
  i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)get_i2c_bus_handle(ESP32_I2C_DEV_NUM);
  if (!bus) {
    i2c_master_bus_config_t cfg = {};
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.i2c_port = ESP32_I2C_DEV_NUM;
    cfg.scl_io_num = ESP32_I2C_SCL_PIN;
    cfg.sda_io_num = ESP32_I2C_SDA_PIN;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) return false;
  }

  gpio_config_t io = {};                         // reset pulse, as in Waveshare's example
  io.mode = GPIO_MODE_OUTPUT;
  io.pin_bit_mask = 1ULL << TP_RST_PIN;
  gpio_config(&io);
  gpio_set_level((gpio_num_t)TP_RST_PIN, 1);
  delay(50);
  gpio_set_level((gpio_num_t)TP_RST_PIN, 0);
  delay(50);
  gpio_set_level((gpio_num_t)TP_RST_PIN, 1);
  delay(150);

  if (i2c_master_probe(bus, FT6336_ADDR, 100) != ESP_OK) return false;   // no answer: a non-touch board

  i2c_device_config_t dev = {};
  dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev.device_address = FT6336_ADDR;
  dev.scl_speed_hz = 400000;
  return i2c_master_bus_add_device(bus, &dev, &touch_dev) == ESP_OK;
}

bool touch_read(uint16_t *x, uint16_t *y)
{
  if (!touch_dev) return false;
  uint8_t reg = 0x02;                            // TD_STATUS: number of touch points, then X/Y of the first
  uint8_t d[5] = {0};
  if (i2c_master_transmit_receive(touch_dev, &reg, 1, d, sizeof(d), 20) != ESP_OK) return false;
  if ((d[0] & 0x0f) == 0) return false;
  *x = ((uint16_t)(d[1] & 0x0f) << 8) | d[2];
  *y = ((uint16_t)(d[3] & 0x0f) << 8) | d[4];
  if (*x > 199) *x = 199;
  if (*y > 199) *y = 199;
  return true;
}
