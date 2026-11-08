#pragma once
#include <stdint.h>
int android_audio_open(void);
void android_audio_reset(void);
void android_audio_remove(uint32_t id);
int android_audio_buffer(uint32_t id, const void *pcm, uint32_t size, uint16_t channels, uint16_t bits, uint32_t rate);
void android_audio_control(uint32_t id, int playing, int loop, uint32_t position, int volume, int pan, uint32_t frequency);
uint32_t android_audio_position(uint32_t id, int *playing);
int android_audio_selftest(void);
