// main/profile_store.c
#include "profile_store.h"

#include <string.h>

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "profile";
static const char *NS = "passport";

static bool s_nvs_ready;

static const char *STYLE_NAMES[PROFILE_STYLE_COUNT] = {
    "PIXEL", "REAL", "E-INK", "SKETCH", "CREAM",
};

static const char *NICK_PRESETS[PROFILE_NICK_PRESETS] = {
    "ACE", "TANKER", "PASSPORT", "NOVA", "FOLO",
};

static const char *SIGN_PRESETS[PROFILE_SIGN_PRESETS] = {
    "Wear it. Flash it.",
    "Pocket tank commander",
    "Made for ACE",
    "Tap to say hi",
    "Ready when you are",
};

static const char *URL_PRESETS[PROFILE_URL_PRESETS] = {
    "https://ai-passport.folotoy.cn",
    "https://folotoy.cn",
    "https://github.com/FoloToy/ai-passport",
};

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

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) src = "";
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
    // 不允许把 UTF-8 多字节字截成半个，否则中文会显示成方块。
    size_t n = strlen(dst);
    if (n && (dst[n - 1] & 0x80)) {
        while (n && ((uint8_t)dst[n - 1] & 0xC0) == 0x80) n--;
        if (n && ((uint8_t)dst[n - 1] & 0xE0) == 0xC0) n--;
        else if (n && ((uint8_t)dst[n - 1] & 0xF0) == 0xE0) n--;
        else if (n && ((uint8_t)dst[n - 1] & 0xF8) == 0xF0) n--;
        dst[n] = '\0';
    }
}

const char *profile_style_name(uint8_t style) {
    return STYLE_NAMES[style % PROFILE_STYLE_COUNT];
}

const char *profile_nick_preset(uint8_t index) {
    return NICK_PRESETS[index % PROFILE_NICK_PRESETS];
}

const char *profile_sign_preset(uint8_t index) {
    return SIGN_PRESETS[index % PROFILE_SIGN_PRESETS];
}

const char *profile_url_preset(uint8_t index) {
    return URL_PRESETS[index % PROFILE_URL_PRESETS];
}

uint8_t profile_next_preset(uint8_t current, uint8_t count) {
    if (count == 0) return 0;
    return (uint8_t)((current + 1u) % count);
}

profile_url_result_t profile_check_url(const char *url, size_t *out_len) {
    if (!url) return PROFILE_URL_EMPTY;
    const size_t len = strlen(url);
    if (out_len) *out_len = len;
    if (len == 0) return PROFILE_URL_EMPTY;
    if (len > PROFILE_URL_MAX) return PROFILE_URL_TOO_LONG;
    const bool http = strncmp(url, "http://", 7) == 0;
    const bool https = strncmp(url, "https://", 8) == 0;
    if (!http && !https) return PROFILE_URL_BAD_SCHEME;
    return PROFILE_URL_OK;
}

static void load_defaults(profile_t *p) {
    memset(p, 0, sizeof(*p));
    p->style = 0;
    p->avatar = 0;
    p->volume = 25;
    p->bright = 100;
    p->idle = 30;
    copy_str(p->petname, sizeof(p->petname), PROFILE_PETNAME_DEFAULT);
    copy_str(p->nick, sizeof(p->nick), NICK_PRESETS[0]);
    copy_str(p->sign, sizeof(p->sign), SIGN_PRESETS[0]);
    copy_str(p->url, sizeof(p->url), URL_PRESETS[0]);
}

void profile_load(profile_t *out) {
    if (!out) return;
    load_defaults(out);
    if (!ensure_nvs()) return;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;

    uint8_t u8 = 0;
    if (nvs_get_u8(h, "style", &u8) == ESP_OK && u8 < PROFILE_STYLE_COUNT) out->style = u8;
    if (nvs_get_u8(h, "avatar", &u8) == ESP_OK && u8 < PROFILE_AVATAR_COUNT) out->avatar = u8;
    if (nvs_get_u8(h, "vol", &u8) == ESP_OK && u8 <= 100) out->volume = u8;
    if (nvs_get_u8(h, "bl", &u8) == ESP_OK && u8 <= 100) out->bright = u8;
    if (nvs_get_u8(h, "idle", &u8) == ESP_OK && (u8 == 0 || u8 == 15 || u8 == 30)) {
        out->idle = u8;
    }
    // 单衣柜。旧档只有兽形/人形两套（ob / oh），按新键 → ob → oh 顺序兼容读取，
    // 读不到就保持默认值，不报错也不清档。
    if (nvs_get_u8(h, "outfit", &u8) == ESP_OK && u8 < PROFILE_OUTFIT_COUNT) {
        out->outfit = u8;
    } else if (nvs_get_u8(h, "ob", &u8) == ESP_OK && u8 < PROFILE_OUTFIT_COUNT) {
        out->outfit = u8;
    } else if (nvs_get_u8(h, "oh", &u8) == ESP_OK && u8 < PROFILE_OUTFIT_COUNT) {
        out->outfit = u8;
    }

    if (nvs_get_u8(h, "acc", &u8) == ESP_OK && u8 <= 3) {
        out->acc = u8;
    } else if (nvs_get_u8(h, "glass", &u8) == ESP_OK && u8 <= 1) {
        // 旧档只有"戴/不戴眼镜"，映射到新枚举的 1=眼镜。
        out->acc = u8 ? 1 : 0;
    }
    if (nvs_get_u8(h, "hero", &u8) == ESP_OK && u8 <= 1) out->hero = u8;
    if (nvs_get_u8(h, "sex", &u8) == ESP_OK && u8 <= 1) out->sex = u8;
    if (nvs_get_u8(h, "lang", &u8) == ESP_OK && u8 <= 1) out->lang = u8;
    // 主题：新键优先，读不到/越界保持默认 0，不动旧档其他字段。
    if (nvs_get_u8(h, "theme", &u8) == ESP_OK && u8 < PROFILE_THEME_COUNT) out->theme = u8;

    char buf[PROFILE_URL_MAX + 1];
    size_t len = sizeof(buf);
    if (nvs_get_str(h, "nick", buf, &len) == ESP_OK && buf[0]) {
        copy_str(out->nick, sizeof(out->nick), buf);
    }
    len = sizeof(buf);
    if (nvs_get_str(h, "petname", buf, &len) == ESP_OK && buf[0]) {
        copy_str(out->petname, sizeof(out->petname), buf);
    }
    len = sizeof(buf);
    if (nvs_get_str(h, "sign", buf, &len) == ESP_OK) {
        copy_str(out->sign, sizeof(out->sign), buf);
    }
    len = sizeof(buf);
    if (nvs_get_str(h, "url", buf, &len) == ESP_OK && buf[0]) {
        copy_str(out->url, sizeof(out->url), buf);
    }
    nvs_close(h);
}

void profile_save(const profile_t *p) {
    if (!p || !ensure_nvs()) return;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "打开 NVS 失败，设置未保存");
        return;
    }
    nvs_set_u8(h, "style", p->style % PROFILE_STYLE_COUNT);
    nvs_set_u8(h, "avatar", p->avatar % PROFILE_AVATAR_COUNT);
    nvs_set_u8(h, "vol", p->volume > 100 ? 100 : p->volume);
    nvs_set_u8(h, "bl", p->bright > 100 ? 100 : p->bright);
    nvs_set_u8(h, "idle", p->idle);
    nvs_set_u8(h, "outfit", p->outfit % PROFILE_OUTFIT_COUNT);
    nvs_set_u8(h, "acc", p->acc > 3 ? 0 : p->acc);
    nvs_set_u8(h, "hero", p->hero ? 1 : 0);
    nvs_set_u8(h, "sex", p->sex ? 1 : 0);
    nvs_set_u8(h, "theme", p->theme % PROFILE_THEME_COUNT);
    nvs_set_u8(h, "lang", p->lang ? 1 : 0);
    nvs_set_str(h, "petname", p->petname);
    nvs_set_str(h, "nick", p->nick);
    nvs_set_str(h, "sign", p->sign);
    nvs_set_str(h, "url", p->url);
    const esp_err_t err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "提交 NVS 失败: %s", esp_err_to_name(err));
}

uint32_t profile_load_best(void) {
    if (!ensure_nvs()) return 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
    uint32_t best = 0;
    (void)nvs_get_u32(h, "best", &best);
    nvs_close(h);
    return best;
}

void profile_save_best(uint32_t score) {
    if (!ensure_nvs()) return;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "best", score);
    (void)nvs_commit(h);
    nvs_close(h);
}

// 分块头像的提交标志：最后一条写，写了才算“这张图完整”。
// bank 0 用 av0..av5，bank 1 用 aw0..aw5；meta 里存生效 bank + 总长 + CRC32。
#define AV_META_KEY "avm"
#define AV_META_MAGIC 0x324D5641u  // "AVM2"

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t bank;
    uint8_t reserved[3];
    uint32_t len;
    uint32_t crc;
} av_meta_t;

static void av_seg_key(char *k, uint8_t bank, uint8_t idx) {
    k[0] = 'a';
    k[1] = bank ? 'w' : 'v';
    k[2] = (char)('0' + idx);
    k[3] = '\0';
}

static bool av_meta_get(av_meta_t *m) {
    if (!ensure_nvs()) return false;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof(*m);
    const esp_err_t e = nvs_get_blob(h, AV_META_KEY, m, &len);
    nvs_close(h);
    return e == ESP_OK && len == sizeof(*m) && m->magic == AV_META_MAGIC &&
           m->bank < PROFILE_AVATAR_BANKS && m->len == PROFILE_AVATAR_BYTES;
}

static bool av_legacy_ok(void) {
    if (!ensure_nvs()) return false;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0;
    const bool ok = nvs_get_blob(h, "ava", NULL, &len) == ESP_OK && len == PROFILE_AVATAR_BYTES;
    nvs_close(h);
    return ok;
}

uint8_t profile_avatar_bank_in_use(void) {
    av_meta_t m;
    return av_meta_get(&m) ? m.bank : PROFILE_AVATAR_BANK_NONE;
}

int profile_avatar_write_seg(uint8_t bank, uint8_t idx, const uint8_t *data, size_t len) {
    if (bank >= PROFILE_AVATAR_BANKS || idx >= PROFILE_AVATAR_SEGS || !data ||
        len != PROFILE_AVATAR_SEG_BYTES || !ensure_nvs()) {
        return ESP_ERR_INVALID_ARG;
    }
    char key[4];
    av_seg_key(key, bank, idx);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    esp_err_t e = nvs_set_blob(h, key, data, len);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

int profile_avatar_commit(uint8_t bank, uint32_t len, uint32_t crc) {
    if (bank >= PROFILE_AVATAR_BANKS || len != PROFILE_AVATAR_BYTES || !ensure_nvs()) {
        return ESP_ERR_INVALID_ARG;
    }
    const av_meta_t m = {AV_META_MAGIC, bank, {0, 0, 0}, len, crc};
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    esp_err_t e = nvs_set_blob(h, AV_META_KEY, &m, sizeof(m));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

size_t profile_load_avatar(uint8_t *buf, size_t cap) {
    if (!buf || cap < PROFILE_AVATAR_BYTES || !ensure_nvs()) return 0;

    // 1) 分块档案：meta 是提交标志。先看 meta 拿到生效 bank，再按 av0..av5（或 aw0..aw5）
    //    顺序拼回来，最后用 CRC 复核。任何一步不符都当“没有头像”，绝不拿半张图去画。
    av_meta_t m;
    if (av_meta_get(&m)) {
        nvs_handle_t h;
        if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
            bool ok = true;
            for (uint8_t i = 0; i < PROFILE_AVATAR_SEGS && ok; i++) {
                char key[4];
                av_seg_key(key, m.bank, i);
                size_t n = PROFILE_AVATAR_SEG_BYTES;
                ok = nvs_get_blob(h, key, buf + (size_t)i * PROFILE_AVATAR_SEG_BYTES, &n) == ESP_OK &&
                     n == PROFILE_AVATAR_SEG_BYTES;
            }
            nvs_close(h);
            if (ok) {
                const uint32_t crc = esp_rom_crc32_le(0, buf, PROFILE_AVATAR_BYTES);
                if (crc == m.crc) return PROFILE_AVATAR_BYTES;
                ESP_LOGW(TAG, "avatar 校验失败(存 %08x / 算 %08x)，按无头像处理",
                         (unsigned)m.crc, (unsigned)crc);
            } else {
                ESP_LOGW(TAG, "avatar 分块缺失，按无头像处理");
            }
        }
    }

    // 2) 旧版单条 ava（23,232 B）：只有分块档案不可用时才走这里，读到就用。
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
    size_t len = cap;
    const esp_err_t e = nvs_get_blob(h, "ava", buf, &len);
    nvs_close(h);
    if (e != ESP_OK) return 0;
    // 只认 88x88 真彩。旧 512 B / 2,192 B 存档一律当作“无头像”：不清档、不当新格式读。
    if (len != PROFILE_AVATAR_BYTES) {
        ESP_LOGW(TAG, "avatar stored len=%u != %u, treated as none",
                 (unsigned)len, (unsigned)PROFILE_AVATAR_BYTES);
        return 0;
    }
    return len;
}

bool profile_avatar_custom(void) {
    av_meta_t m;
    if (av_meta_get(&m)) return true;
    return av_legacy_ok();
}
