// main/profile_store.c
#include "profile_store.h"

#include <string.h>

#include "esp_log.h"
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
    p->volume = 70;
    p->bright = 100;
    p->idle = 30;
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
    if (nvs_get_u8(h, "pstyle", &u8) == ESP_OK && u8 < PROFILE_PSTYLE_COUNT) out->pstyle = u8;
    if (nvs_get_u8(h, "vol", &u8) == ESP_OK && u8 <= 100) out->volume = u8;
    if (nvs_get_u8(h, "bl", &u8) == ESP_OK && u8 <= 100) out->bright = u8;
    if (nvs_get_u8(h, "idle", &u8) == ESP_OK && (u8 == 0 || u8 == 15 || u8 == 30)) {
        out->idle = u8;
    }

    char buf[PROFILE_URL_MAX + 1];
    size_t len = sizeof(buf);
    if (nvs_get_str(h, "nick", buf, &len) == ESP_OK && buf[0]) {
        copy_str(out->nick, sizeof(out->nick), buf);
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
    nvs_set_u8(h, "pstyle", p->pstyle % PROFILE_PSTYLE_COUNT);
    nvs_set_u8(h, "vol", p->volume > 100 ? 100 : p->volume);
    nvs_set_u8(h, "bl", p->bright > 100 ? 100 : p->bright);
    nvs_set_u8(h, "idle", p->idle);
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

bool profile_load_avatar(uint8_t *buf) {
    if (!buf || !ensure_nvs()) return false;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = PROFILE_AVATAR_BYTES;
    const bool ok = (nvs_get_blob(h, "ava", buf, &len) == ESP_OK && len == PROFILE_AVATAR_BYTES);
    nvs_close(h);
    return ok;
}

bool profile_save_avatar(const uint8_t *buf) {
    if (!buf || !ensure_nvs()) return false;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    const bool ok = (nvs_set_blob(h, "ava", buf, PROFILE_AVATAR_BYTES) == ESP_OK &&
                     nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

bool profile_avatar_custom(void) {
    if (!ensure_nvs()) return false;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0;
    const bool ok = (nvs_get_blob(h, "ava", NULL, &len) == ESP_OK &&
                     len == PROFILE_AVATAR_BYTES);
    nvs_close(h);
    return ok;
}
