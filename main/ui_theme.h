// main/ui_theme.h
// 全局唯一视觉规范：深石墨机身 + 暖纸卡面 + 安全橙点色。
// 所有页面（首页/宠物/菜单/设置/门户/弹窗/状态）只从这里取色与尺寸，
// 纯平涂明暗分区，不使用渐变叠色、发光或霓虹描边。
#pragma once

// ---- 色板 ---------------------------------------------------------------
#define U_BG        0x0E1418  // 机身底色
#define U_BG2       0x121A1F  // 机身次层（分区，不做高对比渐变）
#define U_PANEL     0x182227  // 面板/卡片
#define U_PANEL2    0x1F2B31  // 面板浮起层（选中/提示）
#define U_PAPER     0xEDE8DC  // 证件纸
#define U_INK       0x151B1E  // 纸面墨色
#define U_INK2      0x3A464C  // 纸面次级文字
#define U_TEXT      0xE7E2D6  // 深底主文字
#define U_MUTED     0x8D9BA1  // 深底次级文字/刻度
#define U_LINE      0x2A373E  // 深底分隔线
#define U_LINE_SOFT 0x1E282E  // 底纹栅格
#define U_PAPER_LINE 0xC2BAA7 // 纸面分隔线
#define U_ACCENT    0xE0703A  // 安全橙（唯一点色）
#define U_ACCENT_DIM 0x3A2418 // 橙的暗底/选中底
#define U_TRACK     0x222E34  // 进度/条码底槽
#define U_DANGER    0xFF8A80

// ---- 度量 ---------------------------------------------------------------
#define U_GRID      4   // 基础栅格（所有坐标与间距为其倍数）
#define U_GUTTER    16  // 页面左右留白
#define U_HAIR      1   // 分隔线/刻度线宽
#define U_RADIUS    0   // 证件与面板一律直角；仅提示条可用 2px
#define U_ANIM_MS   180 // 交互动效上限（≤200ms，只位移/裁切）
