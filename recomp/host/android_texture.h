#pragma once
#include <stdint.h>
int android_texture_rgba(uint32_t format, unsigned width, unsigned height, unsigned pitch,
                         const uint8_t *source, uint8_t *rgba);
int android_texture_selftest(void);
