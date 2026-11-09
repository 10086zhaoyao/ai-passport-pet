// main/pet_bg.h
// 召唤场景的天气/时段背景层：程序化绘制，对象只在开机创建一次。
// 时间由调用方给出 Unix 秒（UTC）；未授时传 -1，使用安全的默认背景。
#pragma once

#include <stdint.h>

#include "lvgl.h"
#include "weather.h"

// 在宠物场景屏幕创建后、其他 UI 之前调用。背景层占屏幕上部 0..195。
void pet_bg_create(lv_obj_t *parent);

// 每帧调用（仅宠物场景可见时）。epoch 为 UTC 秒，<0 表示未授时。
void pet_bg_tick(uint32_t dt_ms, int64_t epoch, wx_kind_t weather);
