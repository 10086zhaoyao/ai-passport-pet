// main/pet_store.c
#include "pet_store.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "pet_store";
static const char *NS = "pet";
static const char *SLOT_KEY[2] = {"slotA", "slotB"};

#define PET_MAGIC 0x50455431u  // "PET1"
#define PET_VER 1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t seq;
    uint32_t crc;
    uint64_t last_act_utc;
    uint32_t growth;
    uint8_t mood;
    uint8_t fullness;
    uint8_t form;
    uint8_t stage;
    uint8_t reserved[8];
} pet_blob_t;

static bool s_nvs_ready;

static bool ensure_nvs(void) {
    if (s_nvs_ready) return true;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        if (nvs_flash_erase() != ESP_OK) return false;
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 初始化失败: %s", esp_err_to_name(err));
        return false;
    }
    s_nvs_ready = true;
    return true;
}

static uint32_t blob_crc(const pet_blob_t *b) {
    const uint8_t *p = (const uint8_t *)&b->last_act_utc;
    return esp_rom_crc32_le(0, p, (uint32_t)(sizeof(*b) - offsetof(pet_blob_t, last_act_utc)));
}

static bool blob_valid(const pet_blob_t *b) {
    if (b->magic != PET_MAGIC || b->version != PET_VER || b->size != sizeof(*b)) return false;
    if (b->crc != blob_crc(b)) return false;
    if (b->mood > 100 || b->fullness > 100) return false;
    if (b->form > PET_FORM_HUMAN || b->stage >= PET_STAGE_COUNT) return false;
    return true;
}

static bool read_slot(nvs_handle_t h, const char *key, pet_blob_t *b) {
    size_t len = sizeof(*b);
    memset(b, 0, sizeof(*b));
    if (nvs_get_blob(h, key, b, &len) != ESP_OK) return false;
    if (len != sizeof(*b)) return false;
    return blob_valid(b);
}

static void blob_to_state(const pet_blob_t *b, pet_state_t *s) {
    memset(s, 0, sizeof(*s));
    s->growth = b->growth;
    s->mood = b->mood;
    s->fullness = b->fullness;
    s->form = b->form;
    s->stage = pet_stage_for_growth(b->growth);  // 阶段一律由成长值重算
    s->last_act_utc = b->last_act_utc;
    if (!pet_human_unlocked(s)) s->form = PET_FORM_BEAST;
}

void pet_store_load(pet_state_t *out, pet_load_info_t *info) {
    if (!out) return;
    pet_state_default(out);
    if (info) memset(info, 0, sizeof(*info));
    if (!ensure_nvs()) {
        if (info) info->both_bad = true;
        return;
    }

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        if (info) info->both_bad = true;
        return;
    }

    pet_blob_t b[2];
    const bool ok0 = read_slot(h, SLOT_KEY[0], &b[0]);
    const bool ok1 = read_slot(h, SLOT_KEY[1], &b[1]);
    nvs_close(h);

    if (!ok0 && !ok1) {
        if (info) info->both_bad = true;
        ESP_LOGW(TAG, "两份存档都无效，使用默认宠物");
        return;
    }

    int pick;
    if (ok0 && ok1) {
        pick = (b[0].seq >= b[1].seq) ? 0 : 1;
        if (info) info->used_fallback = false;
    } else {
        pick = ok0 ? 0 : 1;
        if (info) info->used_fallback = true;
        ESP_LOGW(TAG, "槽 %d 损坏，回退到槽 %d", ok0 ? 1 : 0, pick);
    }

    blob_to_state(&b[pick], out);
    if (info) info->had_valid = true;
    ESP_LOGI(TAG, "读档: 槽%d seq=%lu growth=%lu stage=%u",
             pick, (unsigned long)b[pick].seq, (unsigned long)out->growth,
             (unsigned)out->stage);
}

bool pet_store_save(const pet_state_t *s) {
    if (!s || !ensure_nvs()) return false;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "打开 NVS 失败");
        return false;
    }

    pet_blob_t old[2] = {0};
    const bool ok0 = read_slot(h, SLOT_KEY[0], &old[0]);
    const bool ok1 = read_slot(h, SLOT_KEY[1], &old[1]);

    // 目标槽：无效槽优先，否则写 seq 较小的槽（保证最新一份始终留在另一槽）。
    int target;
    uint32_t max_seq = 0;
    if (!ok0) {
        target = 0;
    } else if (!ok1) {
        target = 1;
    } else {
        target = (old[0].seq <= old[1].seq) ? 0 : 1;
    }
    if (ok0 && old[0].seq > max_seq) max_seq = old[0].seq;
    if (ok1 && old[1].seq > max_seq) max_seq = old[1].seq;

    pet_blob_t b;
    memset(&b, 0, sizeof(b));
    b.magic = PET_MAGIC;
    b.version = PET_VER;
    b.size = (uint16_t)sizeof(b);
    b.seq = max_seq + 1;
    b.last_act_utc = s->last_act_utc;
    b.growth = s->growth;
    b.mood = s->mood;
    b.fullness = s->fullness;
    b.form = s->form;
    b.stage = pet_stage_for_growth(s->growth);
    b.crc = blob_crc(&b);

    bool ok = (nvs_set_blob(h, SLOT_KEY[target], &b, sizeof(b)) == ESP_OK) &&
              (nvs_commit(h) == ESP_OK);
    if (ok) {
        pet_blob_t check;
        ok = read_slot(h, SLOT_KEY[target], &check) && check.seq == b.seq &&
             check.crc == b.crc;
    }
    nvs_close(h);

    if (!ok) {
        ESP_LOGW(TAG, "写槽 %d 失败，旧存档保留", target);
    }
    return ok;
}
