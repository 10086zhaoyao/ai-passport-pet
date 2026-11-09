// main/profile_store.h
// 首页资料与偏好设置的 NVS 持久化层：风格、头像、昵称、签名、名片短链、最高分。
// 全部字段都是 ASCII，避免在 14/20px 英文字体上出现缺字方块。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROFILE_STYLE_COUNT 5
#define PROFILE_AVATAR_COUNT 3
#define PROFILE_OUTFIT_COUNT 4  // 小猫衣柜：只有一套，4 种配色
#define PROFILE_THEME_COUNT 4   // 首页整卡主题：暖纸/深色/酷卡/彩色水墨
#define PROFILE_NICK_LEN 40   // 12 个汉字（UTF-8 每字 3 字节）+ 结尾
#define PROFILE_SIGN_LEN 80   // 24 个汉字 + 结尾
// 宠物名：UTF-8，上限 24 字节（8 个汉字）+ 结尾，空串合法（渲染时回落默认值）。
// 配网页限 6 汉字 / 12 字母；界面切英文时这个名字也不翻译，始终按中文原样显示。
#define PROFILE_PETNAME_LEN 25
#define PROFILE_PETNAME_DEFAULT "小糯米"

// NTAG213 用户区 144 字节，NDEF URI 记录头约 13 字节，故 URL 上限取 100 字符。
#define PROFILE_URL_MAX 100

typedef struct {
    uint8_t style;   // 0..PROFILE_STYLE_COUNT-1
    uint8_t avatar;  // 0..PROFILE_AVATAR_COUNT-1
    uint8_t volume;  // 0..100，0 为真静音
    uint8_t bright;  // 0..100，LEDC 背光
    uint8_t idle;    // 息屏等待秒数：0=关闭，15，30
    // 小猫衣柜（只有一套）。放在 passport 命名空间，
    // 不触碰宠物的 A/B + CRC 存档结构。
    uint8_t outfit;  // 0..PROFILE_OUTFIT_COUNT-1（旧档保留字段，当前不参与渲染）
    // 配件单值枚举：0=不戴 1=眼镜 2=上班族 3=酷。
    // 单值天然保证"眼镜 / 酷墨镜互斥、同一时刻只戴一件"，旧档 glass=1 映射到 1。
    uint8_t acc;
    // 主人形象：0=男生（默认），1=女生。配网页选择，未上传头像时卡面回落用它。
    uint8_t sex;
    // 主视觉：0=STAGE 显示猫，1=STAGE 显示头像（设置页切换，独立持久化）。
    uint8_t hero;
    uint8_t theme;   // 0..PROFILE_THEME_COUNT-1（旧档缺省按 0：工业灰）
    // 界面语言：0=中文（默认），1=English。只影响界面文案，
    // 宠物名/昵称/签名与 80 条气泡始终按用户原文显示，不翻译。
    uint8_t lang;
    char petname[PROFILE_PETNAME_LEN];
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

// 自定义头像：88x88 真彩，23,232 B，布局与 LV_COLOR_FORMAT_RGB565A8 逐字节一致：
//   前 15,488 B = RGB565 小端、行优先、88 像素宽（每像素 2 B）
//   后  7,744 B = A8 alpha 平面
// 主视觉用整数 2 倍（176）；没有 1:1 展示位。
// 只认这一个长度：旧 512 B / 2,192 B 载荷一律按“无头像”处理，让用户重传一次。
#define PROFILE_AVATAR_W 88
#define PROFILE_AVATAR_RGB_BYTES (PROFILE_AVATAR_W * PROFILE_AVATAR_W * 2)
#define PROFILE_AVATAR_A8_BYTES (PROFILE_AVATAR_W * PROFILE_AVATAR_W)
#define PROFILE_AVATAR_BYTES (PROFILE_AVATAR_RGB_BYTES + PROFILE_AVATAR_A8_BYTES)
#define PROFILE_AVATAR_MAX PROFILE_AVATAR_BYTES

// 读到 buf，返回实际字节数；0 = 没有自定义头像或旧档长度不认识（调用方回落内置形象）。
// 读失败绝不清档。
size_t profile_load_avatar(uint8_t *buf, size_t cap);

// 分块上传：6 段 × 3,872 B，收一块落一块，全程不需要 23,232 B 连续内存。
// A/B 双 bank：新图写进未生效的那一组，meta（生效 bank + 总长 + CRC32）最后写，
// 作为提交标志。传一半断开时 meta 仍指向旧 bank，重启后还是旧头像。
// meta 缺失或 CRC 不符即视为“没有头像”。
#define PROFILE_AVATAR_SEGS 6
#define PROFILE_AVATAR_BANKS 2
#define PROFILE_AVATAR_SEG_BYTES (PROFILE_AVATAR_BYTES / PROFILE_AVATAR_SEGS)
_Static_assert(PROFILE_AVATAR_BYTES % PROFILE_AVATAR_SEGS == 0, "avatar 必须能均分成 6 段");
// 当前生效的 bank；还没有可用头像时返回 PROFILE_AVATAR_BANK_NONE。
#define PROFILE_AVATAR_BANK_NONE 0xFF
uint8_t profile_avatar_bank_in_use(void);
int profile_avatar_write_seg(uint8_t bank, uint8_t idx, const uint8_t *data, size_t len);
int profile_avatar_commit(uint8_t bank, uint32_t len, uint32_t crc);
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
