#ifndef AUDIO_BSP_H
#define AUDIO_BSP_H


#ifdef __cplusplus
extern "C" {
#endif

int audio_bsp_init(void);
void audio_bsp_deinit(void);
void i2s_echo(void *arg);
void audio_playback_set_vol(uint8_t vol);

void audio_play_init(void);

void audio_playback_read(void *data_ptr,uint32_t len);

void audio_playback_write(void *data_ptr,uint32_t len);

#ifdef __cplusplus
}
#endif



#endif