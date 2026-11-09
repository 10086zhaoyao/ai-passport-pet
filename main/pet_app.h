// main/pet_app.h
// 电子身份卡 · 宠物养成：设备入口。复用 BSP 显示/按键/音频/电量与 WiFi 门户。
#pragma once

#include <stdint.h>

// 显示与 LVGL 初始化之后调用：读档、建界面、启动按键/音频/主循环。
void pet_app_start(void);

// WiFi 模块 NTP 授时成功后调用（Unix 秒，UTC）。无 RTC，时间只存在于本次开机 RAM。
void pet_app_set_epoch(int64_t epoch);

// 网页端改了昵称/签名后调用，刷新卡片文字（内部加 LVGL 锁）。
void pet_app_refresh(void);

// 联调测试入口（串口 FAP_TEST 转发）：bench/keys/theme20/audio20/soak/lat/clock/navdump/oldblob。
void pet_app_test_cmd(const char *args);

// 截图/联调用：直接切到指定场景，避免人工按键。
// name: "home" | "play" | "nest" | "sleep" | "happy" | "glass"
void pet_app_shot_scene(const char *name);
