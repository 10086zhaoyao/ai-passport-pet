// main/pet_logic.h
// 宠物养成的纯逻辑：成长、阶段、人形态解锁、4 小时冷却判定。
// 不依赖 LVGL / NVS，方便在主机上单测。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 两次互动之间的冷却：4 小时。时间基准是 NTP 墙钟（UTC 秒），不是 uptime。
#define PET_COOLDOWN_SEC (4 * 60 * 60)
#define PET_STAGE_COUNT 4
#define PET_HUMAN_STAGE 3
#define PET_HUMAN_GROWTH 12

typedef enum {
    PET_FORM_BEAST = 0,
    PET_FORM_HUMAN = 1,
} pet_form_t;

typedef enum {
    PET_ACT_PET = 0,   // 摸摸：心情 +
    PET_ACT_FEED,      // 喂养：饱食 +
    PET_ACT_COUNT,
} pet_action_t;

typedef struct {
    uint32_t growth;       // 累计互动次数
    uint8_t mood;          // 0..100
    uint8_t fullness;      // 0..100
    uint8_t form;          // pet_form_t
    uint8_t stage;         // 由 growth 推导，存档时一并保存便于校验
    uint64_t last_act_utc; // 上次互动 UTC 秒；0 = 从未互动
} pet_state_t;

void pet_state_default(pet_state_t *s);

uint8_t pet_stage_for_growth(uint32_t growth);
const char *pet_stage_name(uint8_t stage);
// 当前阶段内的成长进度 0..100（用于进度条）。
uint8_t pet_stage_progress(const pet_state_t *s);
uint32_t pet_level(const pet_state_t *s);
bool pet_human_unlocked(const pet_state_t *s);

// 剩余冷却秒数：
//   0    = 可以互动
//   > 0  = 还需等待
//   -1   = 当前没有可信时间（未校时），无法判定
int32_t pet_cooldown_left(const pet_state_t *s, int64_t now_utc);

// 时间可信且冷却结束时返回 true。
bool pet_can_interact(const pet_state_t *s, int64_t now_utc);

// 执行一次互动。冷却未结束/时间不可信时返回 false，不修改状态。
// evolved 回填本次是否跨阶段（用于播放进化动画）。
bool pet_interact(pet_state_t *s, pet_action_t act, int64_t now_utc, bool *evolved);

// 切换人形/兽形。未解锁返回 false。
bool pet_toggle_form(pet_state_t *s);
