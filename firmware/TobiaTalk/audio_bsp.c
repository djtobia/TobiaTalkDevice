#include <stdio.h>
#include "audio_bsp.h"
#include "freertos/FreeRTOS.h"
#include "src/codec_board/codec_board.h"
#include "src/codec_board/codec_init.h"
#include "src/esp_codec_dev/include/esp_codec_dev.h"
#include "esp_heap_caps.h"


esp_codec_dev_handle_t playback = NULL;
esp_codec_dev_handle_t record = NULL;



void audio_bsp_init(void)
{
  set_codec_board_type("S3_ePaper_1_54");
	codec_init_cfg_t codec_cfg = 
  {
    .in_mode = CODEC_I2S_MODE_STD,
    .out_mode = CODEC_I2S_MODE_STD,
    .in_use_tdm = false,
    .reuse_dev = false,
  };
  ESP_ERROR_CHECK(init_codec(&codec_cfg));
  playback = get_playback_handle();
  record = get_record_handle();
}

void i2s_echo(void *arg)
{
  	esp_codec_dev_set_out_vol(playback, 60.0); //设置100声音大小
  	esp_codec_dev_set_in_gain(record, 15.0);   //设置录音时的增益
  	uint8_t *data_ptr = (uint8_t *)heap_caps_malloc(1024 * sizeof(uint8_t), MALLOC_CAP_SPIRAM);
  	esp_codec_dev_sample_info_t fs = {};
  	  fs.sample_rate = 48000;
  	  fs.channel = 2;
  	  fs.bits_per_sample = 16;
  	esp_codec_dev_open(playback, &fs); //打开播放
  	esp_codec_dev_open(record, &fs);   //打开录音
  	for(;;)
  	{
  	  	if(ESP_CODEC_DEV_OK == esp_codec_dev_read(record, data_ptr, 1024))
  	  	{
  	  	  	esp_codec_dev_write(playback, data_ptr, 1024);
  	  	}
  	}
}

void audio_playback_set_vol(uint8_t vol)
{
  	esp_codec_dev_set_out_vol(playback, vol); //设置60声音大小
}

void audio_play_init(void)
{
	esp_codec_dev_set_out_vol(playback, 100.0); //设置100声音大小
  	esp_codec_dev_set_in_gain(record, 45.0);   //设置录音时的增益
  	esp_codec_dev_sample_info_t fs = {};
  	  fs.sample_rate = 16000;
  	  fs.channel = 2;
  	  fs.bits_per_sample = 16;
  	esp_codec_dev_open(playback, &fs); //打开播放
  	esp_codec_dev_open(record, &fs);   //打开录音
}

void audio_playback_read(void *data_ptr,uint32_t len)
{
	esp_codec_dev_read(record, data_ptr, len);
}

void audio_playback_write(void *data_ptr,uint32_t len)
{
	esp_codec_dev_write(playback, data_ptr, len);
}

