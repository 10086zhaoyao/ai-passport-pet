// main/pet_store.h
// 宠物存档：NVS A/B 双槽 + magic/version/seq/CRC32，防掉电、坏槽可回退。
#pragma once

#include <stdbool.h>

#include "pet_logic.h"

typedef struct {
    bool had_valid;      // 至少有一份有效存档
    bool used_fallback;  // 最新槽损坏，回退到了另一槽
    bool both_bad;       // 两槽都无效（已用默认值）
} pet_load_info_t;

void pet_store_load(pet_state_t *out, pet_load_info_t *info);

// 只在状态变化时调用。写入较旧槽并回读校验；失败时旧槽保持不变。
bool pet_store_save(const pet_state_t *s);
