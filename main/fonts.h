// main/fonts.h
// 由 gen_fonts.py 生成的两套子集字库声明。
// font_bubble_16：气泡文案专用，4bpp，字节数很小。
// font_cjk_16：GB2312 一级常用字 + ASCII + 中文标点，2bpp，用于昵称/签名/任意用户输入。
#pragma once

#include "lvgl.h"

extern const lv_font_t font_bubble_16;
extern const lv_font_t font_cjk_16;
// 24px：首页姓名行（v1.1.0 卡面锚点要求 24px）。
extern const lv_font_t font_cjk_24;
// 8px：场景页底部落款/互动说明（美术终稿要求字号 8）。
extern const lv_font_t font_cjk_8;
// 16px 手写体：仅首页签名行（霞鹜文楷，OFL 1.1，4bpp；见仓库根 OFL-LXGWWenKai.txt）。
extern const lv_font_t font_sign_16;
