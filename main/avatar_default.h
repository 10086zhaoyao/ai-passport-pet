// main/avatar_default.h
#pragma once
#include <stdint.h>

#define AVATAR_DEFAULT_W 88
#define AVATAR_DEFAULT_BYTES 23232

// 内置形象槽：0=男（默认），1=女。两套都是 88x88 RGB565A8 原生数据，同一渲染链路。
extern const uint8_t avatar_default_male_rgb565a8[AVATAR_DEFAULT_BYTES];
extern const uint8_t avatar_default_female_rgb565a8[AVATAR_DEFAULT_BYTES];

// 按 sex（0=男 1=女）取槽；越界回落男版。返回 .rodata 指针，零 DRAM。
static inline const uint8_t *avatar_default_slot(uint8_t sex) {
    return sex ? avatar_default_female_rgb565a8 : avatar_default_male_rgb565a8;
}
