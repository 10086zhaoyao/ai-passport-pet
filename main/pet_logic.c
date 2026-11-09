// main/pet_logic.c
#include "pet_logic.h"

#include <string.h>

static const char *STAGE_NAMES[PET_STAGE_COUNT] = {
    "SPROUT", "SPIRIT", "SAGE", "ASCEND",
};

static const uint32_t STAGE_MIN_GROWTH[PET_STAGE_COUNT] = {
    0, 3, 7, PET_GROWTH_TOP_STAGE,
};

void pet_state_default(pet_state_t *s) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->mood = 60;
    s->fullness = 55;
    s->stage = 0;
}

uint8_t pet_stage_for_growth(uint32_t growth) {
    uint8_t stage = 0;
    for (uint8_t i = 0; i < PET_STAGE_COUNT; i++) {
        if (growth >= STAGE_MIN_GROWTH[i]) stage = i;
    }
    return stage;
}

const char *pet_stage_name(uint8_t stage) {
    return STAGE_NAMES[stage % PET_STAGE_COUNT];
}

int32_t pet_cooldown_left(const pet_state_t *s, int64_t now_utc) {
    if (s->last_act_utc == 0) return 0;
    if (now_utc <= 0) return -1;
    if ((uint64_t)now_utc < s->last_act_utc) {
        // 时钟被回拨：不猜测真实间隔，按完整冷却处理，避免刷奖励。
        return PET_COOLDOWN_SEC;
    }
    const uint64_t elapsed = (uint64_t)now_utc - s->last_act_utc;
    if (elapsed >= PET_COOLDOWN_SEC) return 0;
    return (int32_t)(PET_COOLDOWN_SEC - elapsed);
}

bool pet_can_interact(const pet_state_t *s, int64_t now_utc) {
    return pet_cooldown_left(s, now_utc) == 0;
}

static uint8_t clamp100(int v) {
    if (v < 0) return 0;
    if (v > 100) return 100;
    return (uint8_t)v;
}

bool pet_interact(pet_state_t *s, pet_action_t act, int64_t now_utc, bool *evolved) {
    if (evolved) *evolved = false;
    if (!s || act >= PET_ACT_COUNT) return false;
    if (!pet_can_interact(s, now_utc)) return false;

    const uint8_t old_stage = s->stage;

    s->growth++;
    if (act == PET_ACT_PET) {
        s->mood = clamp100((int)s->mood + 12);
        s->fullness = clamp100((int)s->fullness - 2);
    } else {
        s->fullness = clamp100((int)s->fullness + 25);
        s->mood = clamp100((int)s->mood + 4);
    }
    s->last_act_utc = (uint64_t)now_utc;
    s->stage = pet_stage_for_growth(s->growth);

    if (evolved) *evolved = (s->stage > old_stage);
    return true;
}
