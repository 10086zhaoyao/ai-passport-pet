// main/profile_store.h
// 首页资料与偏好设置的 NVS 持久化层：风格、头像、昵称、签名、名片短链、最高分。
// 全部字段都是 ASCII，避免在 14/20px 英文字体上出现缺字方块。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROFILE_STYLE_COUNT 5
#define PROFILE_AVATAR_COUNT 3
#define PROFILE_PSTYLE_COUNT 2  // 宠物画风：0=Q 版，1=真人
#define PROFILE_NICK_LEN 16
#define PROFILE_SIGN_LEN 40

// NTAG213 用户区 144 字节，NDEF URI 记录头约 13 字节，故 URL 上限取 100 字符。
#define PROFILE_URL_MAX 100

typedef struct {
    uint8_t style;   // 0..PROFILE_STYLE_COUNT-1
    uint8_t avatar;  // 0..PROFILE_AVATAR_COUNT-1
    uint8_t pstyle;  // 宠物画风 0..PROFILE_PSTYLE_COUNT-1（Q 版/真人）
    uint8_t volume;  // 0..100，0 为真静音
    uint8_t bright;  // 0..100，LEDC 背光
    uint8_t idle;    // 息屏等待秒数：0=关闭，15，30
    char nick[PROFILE_NICK_LEN];
    char sign[PROFILE_SIGN_LEN];
    char url[PROFILE_URL_MAX + 1];
} profile_t;

typedef enum {
    PROFILE_URL_OK = 0,
    PROFILE_URL_EMPTY,
    PROFILE_URL_TOO_LONG,
    PROFILE_URL_BAD_SCHEME,
} profile_url_result_t;

// NVS 初始化 + 读取；首次运行返回内置默认值。
void profile_load(profile_t *out);
void profile_save(const profile_t *p);

uint32_t profile_load_best(void);
void profile_save_best(uint32_t score);

// 自定义头像：64x64 1-bit 位图，512 字节，MSB 优先；1 = 墨色。
#define PROFILE_AVATAR_BYTES 512
bool profile_load_avatar(uint8_t *buf);
bool profile_save_avatar(const uint8_t *buf);
bool profile_avatar_custom(void);

// 校验待写入 NDEF 的名片短链；out_len 可为 NULL。
profile_url_result_t profile_check_url(const char *url, size_t *out_len);

const char *profile_style_name(uint8_t style);
const char *profile_nick_preset(uint8_t index);
const char *profile_sign_preset(uint8_t index);
const char *profile_url_preset(uint8_t index);

#define PROFILE_NICK_PRESETS 5
#define PROFILE_SIGN_PRESETS 5
#define PROFILE_URL_PRESETS 3

// 设置页用于"下一个可选项"的循环步进。
uint8_t profile_next_preset(uint8_t current, uint8_t count);
