// themes_info.h —— 由 pack_v110.py 自动生成，请勿手改。
#pragma once
#include <stdint.h>

#define THEME_COUNT 4
#define THEME_W 240
#define THEME_H 320
#define THEME_BYTES 153600

// 首页公共锚点（美术定稿，240x320 绝对坐标）。旧 nick y182 / sign y204 / UNIT y238 作废。
#define HOME_STAGE_X 30
#define HOME_STAGE_Y 46
#define HOME_STAGE_W 180
#define HOME_STAGE_H 196
#define HOME_TIME_X 12
#define HOME_TIME_Y 3
#define HOME_BATT_RIGHT 228
#define HOME_BATT_Y 12
#define HOME_BAND_LINE_Y 298
#define HOME_BAND_Y 298

typedef struct {
    const char *name;   // 设置页显示名（中文）
    uint32_t off;       // themes.bin 内偏移（RGB565 小端，不透明）
    uint32_t bg;        // 机身边框底色
    uint32_t card;      // 卡面底色
    uint32_t ink;       // 卡面主文字
    uint32_t ink2;      // 卡面次级文字
    uint32_t muted;     // 卡面标签/刻度
    uint32_t screen_ink;    // 机身主文字
    uint32_t screen_muted;  // 机身次文字
    uint32_t hdr;       // 抬头带底色
    uint32_t hdr_ink;   // 抬头带文字
    uint32_t accent;    // 唯一点色
} theme_info_t;

static const theme_info_t THEMES[THEME_COUNT] = {
    {"暖白官方", 0, 0xE2D6C2, 0xF2EBDF, 0x2B2620, 0x6B6153, 0x706555, 0x2B2620, 0x6B6153, 0xF2EBDF, 0x2B2620, 0xC4633A},
    {"深色官方", 153600, 0x0E181E, 0x18252E, 0xF2EDE3, 0xC3D0D3, 0x9AA9AD, 0xF2EDE3, 0xC3D0D3, 0x18252E, 0xF2EDE3, 0xE08A4A},
    {"酷黑工业", 307200, 0x0B0E0F, 0xE7EBE4, 0x101414, 0x3F4A45, 0x55635B, 0xE8EAE9, 0x8E9492, 0x15191B, 0xE8EAE9, 0xB6E51B},
    {"彩色水墨", 460800, 0xE7DDC8, 0xF4EFE4, 0x1C1A17, 0x5A5147, 0x655C51, 0x1C1A17, 0x5A5147, 0xF4EFE4, 0x1C1A17, 0xB03A2E}
};

// 逐主题文字锚点：A/B 共用（24,247）/（24,275），酷卡用文字牌（34,252）/（34,281），
// 彩色水墨暂沿用 A/B（美术未出图）。
// 头像框：A/B (182,246)，酷卡整体内移 8px → (174,246)，均 46x46、radius 8、1px 主题描边。
typedef struct {
    int16_t name_x, name_y, name_w;
    int16_t sub_x, sub_y, sub_w;
    int16_t micro_l_x, micro_r_right, micro_y;
    int16_t mark_x, mark_y;
    int16_t av_x, av_y, av_sz;
    uint8_t name_px;
} home_layout_t;

static const home_layout_t HOME_LAYOUT[THEME_COUNT] = {
    {24, 247, 150, 24, 275, 130, 12, 228, 305, 158, 274, 182, 246, 46, 24},
    {24, 247, 150, 24, 275, 130, 12, 228, 305, 158, 274, 182, 246, 46, 24},
    {34, 252, 132, 34, 281, 112, 34, 228, 293, 150, 279, 174, 246, 46, 22},
    {24, 247, 150, 24, 275, 130, 12, 228, 305, 158, 274, 182, 246, 46, 24}
};
