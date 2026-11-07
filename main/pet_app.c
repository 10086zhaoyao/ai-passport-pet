// main/pet_app.c
// 电子身份卡 · 宠物养成。
// 首页 = 官方 PIXEL 模板身份卡（本仓库原厂 home_screen.c 规范）；宠物由 OK 召唤后
// 在独立场景出现并互动。时间基准：NTP 墙钟（UTC），未校时前互动锁定。
#include "pet_app.h"

#include <stdio.h>
#include <string.h>

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "pet_logic.h"
#include "pet_store.h"
#include "profile_store.h"
#include "wifi_portal.h"

static const char *TAG = "pet_app";

#define TICK_MS 33
#define SCREEN_W 240
#define SCREEN_H 320
#define SPARKS 6
#define SET_ROWS 5
#define BRIGHT_MIN 10

// 官方 PIXEL 模板配色（原厂 home_screen.c 出厂默认样式）。
#define T_BG 0xBFE7FF
#define T_BG2 0x9ED8FF
#define T_CARD 0xFFFDF2
#define T_INK 0x14202B
#define T_ACCENT 0xFFB23E
#define T_ON_ACCENT 0x14202B
#define T_BEST 0xA65200
#define T_MUTED 0x4E6579
#define T_DECO 0x82BE2D
#define T_LINE 0x14202B
#define T_TRACK 0xD9E6F0

// 召唤场景：深青玉配色（与首页卡片区分）。
#define P_BG 0x071319
#define P_BG2 0x0B2326
#define P_CARD 0x0E1E26
#define P_EDGE 0x24524B
#define P_INK 0xEAF6F2
#define P_MUTED 0x9FC0BA
#define P_ACCENT 0x4FE0B0
#define P_WARM 0xFFC46B
#define P_DANGER 0xFF8A80
#define P_TRACK 0x1B3540
#define P_INK_SOFT 0x0B2027

enum {
    SND_SUMMON = 1,
    SND_PET,
    SND_FEED,
    SND_EVOLVE,
    SND_DENY,
    SND_UI,
};

typedef struct {
    uint8_t btn;
    uint8_t ev;
} input_ev_t;

typedef enum {
    ANIM_NONE = 0,
    ANIM_SUMMON,
    ANIM_REACT,
    ANIM_EVOLVE,
} anim_t;

typedef enum {
    VIEW_HOME = 0,
    VIEW_PET,
    VIEW_SET,
    VIEW_PORTAL,
} view_t;

static pet_state_t s_pet;
static profile_t s_profile;

// 首页（官方 PIXEL 模板）
static lv_obj_t *s_home_scr;
static lv_obj_t *s_top_label;
static lv_obj_t *s_batt;
static lv_obj_t *s_bolt;
static lv_obj_t *s_nick;
static lv_obj_t *s_sign;
static lv_obj_t *s_status;
static lv_obj_t *s_lv;
static lv_obj_t *s_stage;
static lv_obj_t *s_grow_fill;
static lv_obj_t *s_grow_label;
static lv_obj_t *s_form_chip;
static lv_obj_t *s_form_text;
static lv_obj_t *s_style_chip;
static lv_obj_t *s_style_text;
static lv_obj_t *s_dots[4];
static lv_obj_t *s_avatar;

// 召唤场景
static lv_obj_t *s_pet_scr;
static lv_obj_t *s_pet_title;
static lv_obj_t *s_pet_batt;
static lv_obj_t *s_pet_root;
static lv_obj_t *s_glow;
static lv_obj_t *s_art;
static lv_obj_t *s_spark[SPARKS];
static lv_obj_t *s_react;
static lv_obj_t *s_mood_fill;
static lv_obj_t *s_full_fill;
static lv_obj_t *s_pet_hint;
static lv_obj_t *s_pet_sub;
static lv_obj_t *s_pet_status;
static lv_obj_t *s_pet_toast_box;
static lv_obj_t *s_pet_toast;
static lv_obj_t *s_flash;

// 设置页
static lv_obj_t *s_set_scr;
static lv_obj_t *s_set_name[SET_ROWS];
static lv_obj_t *s_set_val[SET_ROWS];
static lv_obj_t *s_set_cursor[SET_ROWS];
static lv_obj_t *s_set_toast_box;
static lv_obj_t *s_set_toast;
static uint8_t s_set_cursor_row;

// 门户页
static lv_obj_t *s_portal_scr;
static lv_obj_t *s_portal_ssid;
static lv_obj_t *s_portal_pass;
static lv_obj_t *s_portal_left;
static lv_obj_t *s_portal_toast_box;
static lv_obj_t *s_portal_toast;

static QueueHandle_t s_input_q;
static QueueHandle_t s_snd_q;

static int64_t s_epoch_base;
static int64_t s_epoch_anchor_us;
static bool s_time_known;

static view_t s_view;
static bool s_summoned;
static anim_t s_anim;
static int32_t s_anim_ms;
static int32_t s_bob_ms;
static int32_t s_toast_ms;
static int32_t s_cd_ms;
static int64_t s_last_act_us;

static int s_soc = -1;
static int s_hud_soc = -2;
static int s_batt_mv_avg = -1;
static int s_batt_mv_ref = -1;
static int64_t s_batt_ref_us;
static bool s_charging;

static bool s_dimmed;
static bool s_need_save;
static bool s_set_dirty;

// 设置页方向键去抖：两次导航至少间隔 240ms。
#define NAV_GUARD_US 240000
static int64_t s_nav_us;

// ---------------------------------------------------------------------------
// 基础控件
// ---------------------------------------------------------------------------
static lv_obj_t *mk_box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color,
                        int radius) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    return o;
}

static lv_obj_t *mk_clear(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    return o;
}

static lv_obj_t *mk_text(lv_obj_t *parent, const char *text, const lv_font_t *font,
                         uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static lv_obj_t *mk_ring(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color,
                         int width) {
    lv_obj_t *o = mk_clear(parent, x, y, w, h);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(o, width, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(color), 0);
    return o;
}

static void part(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius) {
    lv_obj_t *o = mk_box(parent, x, y, w, h, color, radius);
    (void)o;
}

// ---------------------------------------------------------------------------
// 宠物绘制：召唤场景 120x160 画布，Q 版与真人两种画风。
// ---------------------------------------------------------------------------
static void draw_beast_q(void) {
    const uint8_t stage = s_pet.stage;
    part(s_art, 30, 150, 60, 5, 0x041016, 2);  // 影子

    if (stage >= 3) {  // 披风
        part(s_art, 16, 80, 16, 52, 0x2E9E7C, 6);
        part(s_art, 88, 80, 16, 52, 0x2E9E7C, 6);
    }
    if (stage >= 3) mk_ring(s_art, 10, 12, 100, 118, P_ACCENT, 2);
    if (stage == 2) mk_ring(s_art, 10, 12, 100, 118, 0x2E9E7C, 1);

    part(s_art, 34, 70, 52, 64, P_ACCENT, 20);      // 圆滚滚身体
    part(s_art, 42, 88, 36, 28, 0xDFFFF4, 12);     // 脸
    part(s_art, 46, 96, 6, 8, P_INK_SOFT, 2);      // 眼
    part(s_art, 68, 96, 6, 8, P_INK_SOFT, 2);
    part(s_art, 42, 108, 7, 4, 0xFF9E9E, 2);       // 腮红
    part(s_art, 71, 108, 7, 4, 0xFF9E9E, 2);
    part(s_art, 57, 114, 6, 3, P_INK_SOFT, 1);     // 嘴
    part(s_art, 38, 128, 18, 8, 0x3FA985, 3);      // 脚
    part(s_art, 64, 128, 18, 8, 0x3FA985, 3);

    switch (stage) {
    case 0:  // 幼灵：新芽
        part(s_art, 57, 50, 6, 16, 0x2C8F6E, 2);
        part(s_art, 63, 44, 14, 8, 0x9CE87A, 3);
        break;
    case 1:  // 小灵兽：耳朵 + 尾巴
        part(s_art, 38, 58, 12, 14, P_ACCENT, 6);
        part(s_art, 70, 58, 12, 14, P_ACCENT, 6);
        part(s_art, 40, 61, 7, 8, 0xFFB7A8, 3);
        part(s_art, 73, 61, 7, 8, 0xFFB7A8, 3);
        part(s_art, 86, 100, 10, 28, 0x3FA985, 5);
        part(s_art, 90, 92, 10, 10, 0x3FA985, 5);
        part(s_art, 57, 84, 6, 6, P_WARM, 3);
        break;
    case 2:  // 灵兽：双角、额印、围巾
        part(s_art, 36, 48, 8, 24, 0xFFD98A, 3);
        part(s_art, 76, 48, 8, 24, 0xFFD98A, 3);
        part(s_art, 56, 76, 8, 8, P_WARM, 4);
        part(s_art, 32, 100, 56, 10, P_WARM, 4);
        part(s_art, 56, 104, 20, 9, 0xFFE3AE, 3);
        part(s_art, 86, 108, 10, 22, 0x3FA985, 5);
        break;
    default:  // 灵君：冠、珠
        part(s_art, 36, 40, 8, 26, 0xFFD98A, 3);
        part(s_art, 56, 36, 8, 30, 0xFFD98A, 3);
        part(s_art, 76, 40, 8, 26, 0xFFD98A, 3);
        part(s_art, 54, 24, 12, 12, P_WARM, 6);
        part(s_art, 30, 106, 60, 9, P_WARM, 4);
        part(s_art, 52, 110, 16, 9, 0x1F7A5E, 5);
        break;
    }
}

static void draw_beast_real(void) {
    const uint8_t stage = s_pet.stage;
    part(s_art, 30, 150, 60, 5, 0x041016, 2);

    part(s_art, 88, 96, 20, 6, 0x3FA985, 3);   // 长尾上卷
    part(s_art, 98, 84, 6, 16, 0x3FA985, 3);
    part(s_art, 44, 124, 9, 24, 0x3FA985, 4);  // 细腿
    part(s_art, 67, 124, 9, 24, 0x3FA985, 4);
    part(s_art, 38, 72, 44, 60, P_ACCENT, 16); // 修长躯干
    part(s_art, 48, 88, 24, 30, 0xDFFFF4, 10);
    part(s_art, 40, 28, 40, 38, P_ACCENT, 18); // 小头
    part(s_art, 42, 14, 7, 16, P_ACCENT, 3);   // 尖耳
    part(s_art, 71, 14, 7, 16, P_ACCENT, 3);
    part(s_art, 46, 44, 28, 16, 0xDFFFF4, 8);
    part(s_art, 50, 49, 4, 5, P_INK_SOFT, 2);  // 细长眼
    part(s_art, 66, 49, 4, 5, P_INK_SOFT, 2);
    part(s_art, 58, 57, 4, 3, 0xC0705A, 1);

    switch (stage) {
    case 0:
        part(s_art, 57, 56, 6, 10, 0x2C8F6E, 2);
        break;
    case 1:
        part(s_art, 38, 64, 44, 7, 0x9CE87A, 3);  // 飘带
        part(s_art, 78, 68, 16, 4, 0x9CE87A, 2);
        break;
    case 2:
        part(s_art, 44, 8, 6, 16, 0xFFD98A, 2);   // 细角
        part(s_art, 70, 8, 6, 16, 0xFFD98A, 2);
        part(s_art, 57, 34, 6, 6, P_WARM, 3);
        part(s_art, 36, 70, 48, 7, P_WARM, 3);    // 围巾
        break;
    default:
        part(s_art, 44, 4, 6, 18, 0xFFD98A, 2);   // 冠
        part(s_art, 57, 0, 6, 22, 0xFFD98A, 2);
        part(s_art, 70, 4, 6, 18, 0xFFD98A, 2);
        part(s_art, 22, 60, 12, 60, 0x2E9E7C, 4);  // 侧披
        part(s_art, 86, 60, 12, 60, 0x2E9E7C, 4);
        part(s_art, 56, 20, 8, 8, P_WARM, 4);      // 灵珠
        part(s_art, 38, 100, 44, 6, P_WARM, 3);
        break;
    }
}

static void draw_human_q(void) {
    part(s_art, 36, 150, 48, 5, 0x041016, 2);
    if (s_pet.growth >= 20) {
        part(s_art, 24, 84, 14, 44, 0x1F7A5E, 4);
        part(s_art, 82, 84, 14, 44, 0x1F7A5E, 4);
    }
    if (s_pet.growth >= 16) mk_ring(s_art, 8, 18, 104, 128, 0x2E9E7C, 1);

    part(s_art, 40, 84, 40, 52, 0x3FC79E, 10);   // 短袍
    part(s_art, 50, 92, 20, 32, 0xE9FFF7, 6);    // 内襟
    part(s_art, 44, 110, 32, 7, P_WARM, 3);      // 腰带
    part(s_art, 32, 88, 14, 28, 0x3FC79E, 6);    // 袖
    part(s_art, 74, 88, 14, 28, 0x3FC79E, 6);
    part(s_art, 34, 114, 10, 8, 0xFFDCC2, 4);    // 手
    part(s_art, 76, 114, 10, 8, 0xFFDCC2, 4);
    part(s_art, 46, 80, 28, 8, 0x2E9E7C, 4);     // 领
    part(s_art, 46, 132, 10, 10, 0xFFDCC2, 4);   // 鞋
    part(s_art, 64, 132, 10, 10, 0xFFDCC2, 4);

    part(s_art, 55, 76, 10, 6, 0xFFDCC2, 3);     // 脖子
    part(s_art, 44, 44, 32, 32, 0xFFDCC2, 14);   // 大头
    part(s_art, 42, 38, 36, 18, 0x1B2B33, 8);    // 发
    part(s_art, 52, 28, 16, 12, 0x1B2B33, 6);    // 发髻
    part(s_art, 50, 58, 4, 6, P_INK_SOFT, 2);    // 眼
    part(s_art, 66, 58, 4, 6, P_INK_SOFT, 2);
    part(s_art, 57, 68, 6, 2, 0xC0705A, 1);
    part(s_art, 58, 122, 4, 6, P_WARM, 2);       // 玉佩

    if (s_pet.growth >= 14) part(s_art, 64, 32, 10, 3, P_WARM, 1);  // 发簪
    if (s_pet.growth >= 16) {
        part(s_art, 44, 132, 32, 3, P_WARM, 1);
        part(s_art, 48, 58, 3, 16, 0x1B2B33, 1);
        part(s_art, 70, 58, 3, 16, 0x1B2B33, 1);
    }
}

static void draw_human_real(void) {
    part(s_art, 34, 150, 52, 5, 0x041016, 2);
    if (s_pet.growth >= 20) {
        part(s_art, 22, 44, 12, 62, 0x1F7A5E, 4);
        part(s_art, 86, 44, 12, 62, 0x1F7A5E, 4);
    }
    if (s_pet.growth >= 16) mk_ring(s_art, 6, 0, 108, 148, 0x2E9E7C, 1);

    part(s_art, 40, 40, 40, 88, 0x3FC79E, 8);    // 长袍
    part(s_art, 36, 110, 48, 28, 0x3FC79E, 6);   // 袍摆
    part(s_art, 50, 44, 20, 34, 0xE9FFF7, 6);    // 内襟
    part(s_art, 44, 76, 32, 6, P_WARM, 3);       // 腰带
    part(s_art, 72, 80, 22, 4, 0x2E9E7C, 2);     // 飘带
    part(s_art, 30, 48, 14, 38, 0x3FC79E, 6);    // 广袖
    part(s_art, 76, 48, 14, 38, 0x3FC79E, 6);
    part(s_art, 32, 84, 9, 8, 0xFFDCC2, 4);
    part(s_art, 79, 84, 9, 8, 0xFFDCC2, 4);
    part(s_art, 48, 138, 9, 8, 0x1B2B33, 3);    // 履
    part(s_art, 64, 138, 9, 8, 0x1B2B33, 3);

    part(s_art, 48, 12, 24, 24, 0xFFDCC2, 11);   // 小头（近五头身）
    part(s_art, 46, 8, 28, 16, 0x1B2B33, 8);     // 发
    part(s_art, 54, 2, 12, 9, 0x1B2B33, 5);      // 顶髻
    part(s_art, 53, 22, 3, 4, P_INK_SOFT, 1);    // 凤眼
    part(s_art, 64, 22, 3, 4, P_INK_SOFT, 1);
    part(s_art, 58, 27, 4, 2, 0xC0705A, 1);

    if (s_pet.growth >= 14) part(s_art, 62, 6, 10, 3, P_WARM, 1);
    if (s_pet.growth >= 16) {
        part(s_art, 44, 106, 32, 3, P_WARM, 1);
        part(s_art, 46, 18, 3, 14, 0x1B2B33, 1);
        part(s_art, 71, 18, 3, 14, 0x1B2B33, 1);
    }
    if (s_pet.growth >= 20) part(s_art, 54, 110, 4, 6, P_WARM, 2);
}

static void draw_pet(void) {
    if (!s_art) return;
    lv_obj_clean(s_art);
    lv_obj_set_style_opa(s_art, LV_OPA_COVER, 0);
    if (s_pet.form == PET_FORM_HUMAN) {
        if (s_profile.pstyle == 1) {
            draw_human_real();
        } else {
            draw_human_q();
        }
    } else if (s_profile.pstyle == 1) {
        draw_beast_real();
    } else {
        draw_beast_q();
    }
}

// ---------------------------------------------------------------------------
// 首页头像：手机上传的 Q 版头像优先，否则画迷你宠物。
// ---------------------------------------------------------------------------
static lv_color32_t c32(uint32_t hex) {
    return lv_color32_make((uint8_t)(hex >> 16), (uint8_t)(hex >> 8), (uint8_t)hex, 0xFF);
}

static void draw_pet_mini(void) {
    static uint8_t bits[PROFILE_AVATAR_BYTES];
    if (!s_avatar) return;
    lv_obj_clean(s_avatar);

    if (profile_load_avatar(bits)) {
        lv_obj_t *cv = lv_canvas_create(s_avatar);
        lv_canvas_set_buffer(cv, bits, 64, 64, LV_COLOR_FORMAT_I1);
        lv_canvas_set_palette(cv, 0, c32(T_CARD));
        lv_canvas_set_palette(cv, 1, c32(T_INK));
        lv_obj_set_pos(cv, 0, 0);
        return;
    }

    const bool real = (s_profile.pstyle == 1);
    const uint8_t stage = s_pet.stage;
    if (s_pet.form == PET_FORM_HUMAN) {
        if (real) {
            part(s_avatar, 24, 8, 16, 16, 0xFFDCC2, 7);
            part(s_avatar, 22, 6, 20, 12, 0x1B2B33, 5);
            part(s_avatar, 30, 2, 8, 6, 0x1B2B33, 3);
            part(s_avatar, 28, 13, 2, 3, P_INK_SOFT, 1);
            part(s_avatar, 34, 13, 2, 3, P_INK_SOFT, 1);
            part(s_avatar, 22, 26, 20, 34, 0x3FC79E, 5);
            part(s_avatar, 26, 38, 12, 5, P_WARM, 2);
        } else {
            part(s_avatar, 18, 12, 28, 28, 0xFFDCC2, 12);
            part(s_avatar, 16, 8, 32, 16, 0x1B2B33, 7);
            part(s_avatar, 26, 2, 12, 8, 0x1B2B33, 4);
            part(s_avatar, 24, 24, 4, 5, P_INK_SOFT, 2);
            part(s_avatar, 36, 24, 4, 5, P_INK_SOFT, 2);
            part(s_avatar, 20, 40, 24, 20, 0x3FC79E, 7);
            part(s_avatar, 24, 48, 16, 5, P_WARM, 2);
        }
        if (s_pet.growth >= 14) part(s_avatar, 40, 8, 8, 2, P_WARM, 1);
        if (s_pet.growth >= 16) part(s_avatar, 24, 56, 16, 2, P_WARM, 1);
        return;
    }

    if (real) {
        part(s_avatar, 20, 42, 22, 18, P_ACCENT, 7);
        part(s_avatar, 18, 16, 26, 24, P_ACCENT, 11);
        part(s_avatar, 20, 8, 6, 12, P_ACCENT, 2);
        part(s_avatar, 36, 8, 6, 12, P_ACCENT, 2);
        part(s_avatar, 22, 24, 20, 12, 0xDFFFF4, 6);
        part(s_avatar, 25, 27, 3, 4, P_INK_SOFT, 1);
        part(s_avatar, 34, 27, 3, 4, P_INK_SOFT, 1);
        part(s_avatar, 46, 44, 12, 4, 0x3FA985, 2);
    } else {
        part(s_avatar, 18, 40, 26, 20, P_ACCENT, 9);
        part(s_avatar, 16, 14, 30, 26, P_ACCENT, 12);
        part(s_avatar, 20, 22, 22, 14, 0xDFFFF4, 7);
        part(s_avatar, 23, 24, 4, 5, P_INK_SOFT, 2);
        part(s_avatar, 33, 24, 4, 5, P_INK_SOFT, 2);
        part(s_avatar, 20, 32, 5, 3, 0xFF9E9E, 1);
        part(s_avatar, 34, 32, 5, 3, 0xFF9E9E, 1);
        part(s_avatar, 47, 44, 12, 6, 0x3FA985, 3);
    }
    switch (stage) {
    case 0:
        part(s_avatar, 28, 4, 4, 10, 0x2C8F6E, 1);
        part(s_avatar, 32, 2, 8, 5, 0x9CE87A, 2);
        break;
    case 1:
        part(s_avatar, 16, 6, 6, 10, P_ACCENT, 2);
        part(s_avatar, 40, 6, 6, 10, P_ACCENT, 2);
        break;
    case 2:
        part(s_avatar, 15, 2, 5, 12, 0xFFD98A, 2);
        part(s_avatar, 42, 2, 5, 12, 0xFFD98A, 2);
        part(s_avatar, 29, 16, 6, 5, P_WARM, 2);
        break;
    default:
        part(s_avatar, 15, 2, 5, 12, 0xFFD98A, 2);
        part(s_avatar, 28, 0, 6, 14, 0xFFD98A, 2);
        part(s_avatar, 42, 2, 5, 12, 0xFFD98A, 2);
        part(s_avatar, 27, 18, 8, 6, P_WARM, 3);
        break;
    }
}

// ---------------------------------------------------------------------------
// 状态与文案
// ---------------------------------------------------------------------------
static int64_t clock_now(void) {
    if (!s_time_known) return -1;
    return s_epoch_base + (esp_timer_get_time() - s_epoch_anchor_us) / 1000000;
}

static void fmt_left(char *buf, size_t n, int32_t sec) {
    const int s = (int)sec;
    if (s >= 3600) {
        snprintf(buf, n, "READY IN %dH %02dM", s / 3600, (s % 3600) / 60);
    } else {
        snprintf(buf, n, "READY IN %dM %02dS", s / 60, s % 60);
    }
}

static void refresh_status(void) {
    if (!s_status) return;
    char b[24];
    int32_t left = pet_cooldown_left(&s_pet, clock_now());
    if (left < 0) {
        snprintf(b, sizeof(b), "NEED TIME SYNC");
    } else if (left > 0) {
        fmt_left(b, sizeof(b), left);
    } else {
        snprintf(b, sizeof(b), "READY");
    }
    const uint32_t color = (left == 0) ? T_BEST : T_MUTED;
    lv_label_set_text(s_status, b);
    lv_obj_set_style_text_color(s_status, lv_color_hex(color), 0);
    if (s_pet_status) {
        lv_label_set_text(s_pet_status, b);
        lv_obj_set_style_text_color(
            s_pet_status, lv_color_hex(left == 0 ? P_ACCENT : P_MUTED), 0);
    }
}

static void refresh_home(void) {
    if (!s_home_scr) return;
    char b[32];

    lv_label_set_text(s_top_label, pet_stage_name(s_pet.stage));
    lv_label_set_text(s_nick, s_profile.nick[0] ? s_profile.nick : "ACE");
    lv_label_set_text(s_sign, s_profile.sign[0] ? s_profile.sign : "Wear it. Flash it.");
    snprintf(b, sizeof(b), "LV %lu", (unsigned long)pet_level(&s_pet));
    lv_label_set_text(s_lv, b);
    lv_label_set_text(s_stage, pet_stage_name(s_pet.stage));
    lv_label_set_text(s_form_text, s_pet.form == PET_FORM_HUMAN ? "HUMAN" : "BEAST");
    lv_label_set_text(s_style_text, s_profile.pstyle == 1 ? "REAL" : "CHIBI");

    const uint8_t prog = pet_stage_progress(&s_pet);
    lv_obj_set_width(s_grow_fill, (188 * prog) / 100);
    static const uint32_t STAGE_NEXT[3] = {3, 7, 12};
    if (s_pet.stage + 1 >= PET_STAGE_COUNT) {
        snprintf(b, sizeof(b), "GROWTH %lu  MAX", (unsigned long)s_pet.growth);
    } else {
        snprintf(b, sizeof(b), "GROWTH %lu / %lu", (unsigned long)s_pet.growth,
                 (unsigned long)STAGE_NEXT[s_pet.stage]);
    }
    lv_label_set_text(s_grow_label, b);
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_color(s_dots[i],
                                   lv_color_hex((uint8_t)i <= s_pet.stage ? T_ACCENT : T_TRACK),
                                   0);
    }
    draw_pet_mini();
    refresh_status();
}

static void refresh_pet_scene(void) {
    if (!s_pet_scr) return;
    char b[40];
    snprintf(b, sizeof(b), "LV %lu   %s", (unsigned long)pet_level(&s_pet),
             pet_stage_name(s_pet.stage));
    lv_label_set_text(s_pet_title, b);
    lv_obj_set_width(s_mood_fill, (138 * s_pet.mood) / 100);
    lv_obj_set_width(s_full_fill, (138 * s_pet.fullness) / 100);
    lv_label_set_text(s_pet_hint, "UP PET    DOWN FEED");
    lv_label_set_text(s_pet_sub, pet_human_unlocked(&s_pet) ? "OK TUCK    HOLD OK FORM"
                                                            : "OK   TUCK");
    refresh_status();
}

static void show_toast(view_t view, const char *text, uint32_t ms) {
    lv_obj_t *box = NULL;
    lv_obj_t *label = NULL;
    if (view == VIEW_PET) {
        box = s_pet_toast_box;
        label = s_pet_toast;
    } else if (view == VIEW_SET) {
        box = s_set_toast_box;
        label = s_set_toast;
    } else if (view == VIEW_PORTAL) {
        box = s_portal_toast_box;
        label = s_portal_toast;
    }
    if (!box || !label) return;
    lv_label_set_text(label, text);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(box);
    s_toast_ms = (int32_t)ms;
}

// ---------------------------------------------------------------------------
// 屏幕切换
// ---------------------------------------------------------------------------
static void enter_view(view_t v) {
    s_view = v;
    if (v == VIEW_HOME) {
        refresh_home();
        lv_screen_load(s_home_scr);
    } else if (v == VIEW_PET) {
        refresh_pet_scene();
        lv_screen_load(s_pet_scr);
    } else if (v == VIEW_SET) {
        lv_screen_load(s_set_scr);
    } else {
        lv_screen_load(s_portal_scr);
    }
}

// ---------------------------------------------------------------------------
// 动画
// ---------------------------------------------------------------------------
static void anim_step(uint32_t dt) {
    s_anim_ms += (int32_t)dt;

    if (s_anim == ANIM_SUMMON) {
        int32_t t = s_anim_ms;
        if (t > 900) t = 900;
        lv_obj_set_pos(s_art, 0, 26 - (26 * t) / 900);
        lv_obj_set_style_opa(s_art, (lv_opa_t)(255 * t / 900), 0);

        lv_obj_remove_flag(s_glow, LV_OBJ_FLAG_HIDDEN);
        const int gw = 42 + (58 * t) / 900;
        const int gh = gw * 86 / 100;
        lv_obj_set_size(s_glow, gw, gh);
        lv_obj_set_pos(s_glow, (120 - gw) / 2, 76 - gh / 2);
        lv_obj_set_style_opa(s_glow, (lv_opa_t)(150 * (900 - t) / 900), 0);

        static const int8_t dx[SPARKS] = {0, 38, 38, 0, -38, -38};
        static const int8_t dy[SPARKS] = {-44, -22, 22, 44, 22, -22};
        for (int i = 0; i < SPARKS; i++) {
            lv_obj_remove_flag(s_spark[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_spark[i], 58 + dx[i] * t / 900 - 2,
                           72 + dy[i] * t / 900 - 2);
            lv_obj_set_style_opa(s_spark[i], (lv_opa_t)(200 * (900 - t) / 900), 0);
        }

        if (s_anim_ms >= 900) {
            lv_obj_add_flag(s_glow, LV_OBJ_FLAG_HIDDEN);
            for (int i = 0; i < SPARKS; i++) lv_obj_add_flag(s_spark[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_opa(s_art, LV_OPA_COVER, 0);
            lv_obj_set_pos(s_art, 0, 0);
            s_summoned = true;
            s_anim = ANIM_NONE;
            s_anim_ms = 0;
            s_bob_ms = 0;
            refresh_pet_scene();
        }
        return;
    }

    if (s_anim == ANIM_REACT) {
        int32_t t = s_anim_ms;
        if (t > 520) t = 520;
        const int lift = (t < 200) ? (10 * t / 200) : (10 * (520 - t) / 320);
        lv_obj_set_pos(s_art, 0, -lift);
        if (s_react) {
            lv_obj_remove_flag(s_react, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_react, 42, 68 - (t * 22 / 520));
            lv_obj_set_style_opa(s_react, (lv_opa_t)(255 * (520 - t) / 520), 0);
        }
        if (s_anim_ms >= 520) {
            lv_obj_set_pos(s_art, 0, 0);
            if (s_react) lv_obj_add_flag(s_react, LV_OBJ_FLAG_HIDDEN);
            s_anim = ANIM_NONE;
            s_anim_ms = 0;
        }
        return;
    }

    if (s_anim == ANIM_EVOLVE) {
        int32_t t = s_anim_ms;
        if (t > 900) t = 900;
        const int opa = (t < 260) ? (t * 90 / 260) : (90 * (900 - t) / 640);
        lv_obj_set_style_opa(s_flash, (lv_opa_t)opa, 0);
        if (s_anim_ms >= 900) {
            lv_obj_set_style_opa(s_flash, LV_OPA_TRANSP, 0);
            lv_obj_add_flag(s_flash, LV_OBJ_FLAG_HIDDEN);
            s_anim = ANIM_NONE;
            s_anim_ms = 0;
        }
    }
}

static void ui_tick(uint32_t dt) {
    if (s_toast_ms > 0) {
        s_toast_ms -= (int32_t)dt;
        if (s_toast_ms <= 0) {
            if (s_view == VIEW_PET && s_pet_toast_box) {
                lv_obj_add_flag(s_pet_toast_box, LV_OBJ_FLAG_HIDDEN);
            } else if (s_view == VIEW_SET && s_set_toast_box) {
                lv_obj_add_flag(s_set_toast_box, LV_OBJ_FLAG_HIDDEN);
            } else if (s_view == VIEW_PORTAL && s_portal_toast_box) {
                lv_obj_add_flag(s_portal_toast_box, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    if (s_anim != ANIM_NONE) {
        anim_step(dt);
    } else if (s_view == VIEW_PET && s_summoned && !s_dimmed) {
        static const int8_t BOB[8] = {0, -1, -2, -3, -3, -2, -1, 0};
        s_bob_ms += (int32_t)dt;
        lv_obj_set_pos(s_art, 0, BOB[(s_bob_ms / 160) % 8]);
    }

    // 每秒：冷却倒计时 + 门户倒计时/超时
    s_cd_ms += (int32_t)dt;
    if (s_cd_ms >= 1000) {
        s_cd_ms = 0;
        if (s_view == VIEW_HOME || s_view == VIEW_PET) {
            refresh_status();
        }
        if (s_view == VIEW_PORTAL) {
            if (wifi_portal_expired()) {
                wifi_portal_close();
                enter_view(VIEW_HOME);
            } else {
                const long long left_us =
                    180000000LL - ((long long)esp_timer_get_time() -
                                   wifi_portal_last_req_us());
                const int sec = left_us > 0 ? (int)(left_us / 1000000) : 0;
                char b[16];
                snprintf(b, sizeof(b), "%d:%02d", sec / 60, sec % 60);
                lv_label_set_text(s_portal_left, b);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 声音 + 交互
// ---------------------------------------------------------------------------
static void play_sound(uint32_t id) {
    if (!s_snd_q || s_profile.volume == 0) return;
    (void)xQueueSend(s_snd_q, &id, 0);
}

static void deny(const char *text) {
    play_sound(SND_DENY);
    show_toast(VIEW_PET, text, 1900);
}

static void do_interact(pet_action_t act) {
    const int64_t now = clock_now();
    int32_t left = pet_cooldown_left(&s_pet, now);
    if (left < 0) {
        deny("NEED TIME SYNC");
        return;
    }
    if (left > 0) {
        char b[24];
        fmt_left(b, sizeof(b), left);
        deny(b);
        return;
    }

    bool evolved = false;
    if (!pet_interact(&s_pet, act, now, &evolved)) {
        deny("NOT READY");
        return;
    }
    s_need_save = true;

    if (evolved) {
        draw_pet();
        lv_obj_remove_flag(s_flash, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(s_flash, LV_OPA_TRANSP, 0);
        s_anim = ANIM_EVOLVE;
        s_anim_ms = 0;
        play_sound(SND_EVOLVE);
        char b[32];
        snprintf(b, sizeof(b), "EVOLVED   %s", pet_stage_name(s_pet.stage));
        show_toast(VIEW_PET, b, 2200);
    } else {
        lv_label_set_text(s_react, act == PET_ACT_PET ? "+MOOD" : "+FULL");
        s_anim = ANIM_REACT;
        s_anim_ms = 0;
        play_sound(act == PET_ACT_PET ? SND_PET : SND_FEED);
    }
    refresh_pet_scene();
}

// ---------------------------------------------------------------------------
// 设置页
// ---------------------------------------------------------------------------
static const char *SET_NAMES[SET_ROWS] = {
    "ART STYLE", "VOLUME", "BRIGHTNESS", "IDLE SCREEN", "PHONE SETUP",
};

static void settings_refresh(void) {
    for (int i = 0; i < SET_ROWS; i++) {
        const char *v = "";
        char b[16];
        switch (i) {
        case 0:
            v = s_profile.pstyle == 1 ? "REAL" : "CHIBI";
            break;
        case 1:
            snprintf(b, sizeof(b), "%u%%", (unsigned)s_profile.volume);
            v = b;
            break;
        case 2:
            snprintf(b, sizeof(b), "%u%%", (unsigned)s_profile.bright);
            v = b;
            break;
        case 3:
            v = s_profile.idle == 0 ? "OFF" : (s_profile.idle == 15 ? "15S" : "30S");
            break;
        default:
            v = "ENTER >";
            break;
        }
        lv_label_set_text(s_set_val[i], v);
        if (s_set_cursor[i]) {
            lv_obj_set_style_bg_color(s_set_cursor[i],
                                      lv_color_hex((uint8_t)i == s_set_cursor_row
                                                       ? T_ACCENT
                                                       : T_TRACK),
                                      0);
        }
    }
}

static bool nav_ready(void) {
    const int64_t now = esp_timer_get_time();
    if (now - s_nav_us < NAV_GUARD_US) return false;
    s_nav_us = now;
    return true;
}

static void settings_change(void) {
    switch (s_set_cursor_row) {
    case 0:
        s_profile.pstyle ^= 1;
        draw_pet();
        draw_pet_mini();
        play_sound(SND_UI);
        break;
    case 1:
        s_profile.volume = (uint8_t)((s_profile.volume + 10) % 110);
        bsp_audio_set_volume(s_profile.volume);
        play_sound(SND_UI);
        break;
    case 2: {
        uint8_t v = s_profile.bright;
        v = (v >= 100) ? BRIGHT_MIN : (uint8_t)(v + 10);
        s_profile.bright = v;
        bsp_display_backlight(v);
        play_sound(SND_UI);
        break;
    }
    case 3:
        s_profile.idle = (s_profile.idle == 0) ? 15 : (s_profile.idle == 15 ? 30 : 0);
        play_sound(SND_UI);
        break;
    default:
        if (wifi_portal_open()) {
            lv_label_set_text(s_portal_ssid, wifi_portal_ssid());
            lv_label_set_text(s_portal_pass, wifi_portal_pass());
            enter_view(VIEW_PORTAL);
        } else {
            show_toast(VIEW_SET, "WIFI BUSY", 1600);
        }
        return;
    }
    s_set_dirty = true;
    settings_refresh();
}

// ---------------------------------------------------------------------------
// 按键
// ---------------------------------------------------------------------------
static void handle_key(uint8_t btn, uint8_t ev) {
    s_last_act_us = esp_timer_get_time();

    if (ev == BSP_BTN_CLICK) {
        switch (s_view) {
        case VIEW_HOME:
            if (btn == BSP_BTN_OK) {
                s_summoned = false;
                s_anim = ANIM_NONE;
                draw_pet();
                lv_obj_set_pos(s_art, 0, 26);
                lv_obj_set_style_opa(s_art, LV_OPA_TRANSP, 0);
                lv_obj_add_flag(s_react, LV_OBJ_FLAG_HIDDEN);
                enter_view(VIEW_PET);
                s_anim = ANIM_SUMMON;
                s_anim_ms = 0;
                play_sound(SND_SUMMON);
            }
            break;
        case VIEW_PET:
            if (s_anim == ANIM_SUMMON || s_anim == ANIM_EVOLVE) break;
            if (btn == BSP_BTN_UP && s_summoned) {
                do_interact(PET_ACT_PET);
            } else if (btn == BSP_BTN_DOWN && s_summoned) {
                do_interact(PET_ACT_FEED);
            } else if (btn == BSP_BTN_OK && s_summoned) {
                s_summoned = false;
                s_anim = ANIM_NONE;
                play_sound(SND_UI);
                enter_view(VIEW_HOME);
            }
            break;
        case VIEW_SET:
            if (btn == BSP_BTN_UP) {
                if (nav_ready()) {
                    s_set_cursor_row = (uint8_t)((s_set_cursor_row + SET_ROWS - 1) %
                                                 SET_ROWS);
                    settings_refresh();
                }
            } else if (btn == BSP_BTN_DOWN) {
                if (nav_ready()) {
                    s_set_cursor_row = (uint8_t)((s_set_cursor_row + 1) % SET_ROWS);
                    settings_refresh();
                }
            } else if (btn == BSP_BTN_OK) {
                settings_change();
            }
            break;
        case VIEW_PORTAL:
            if (btn == BSP_BTN_OK) {
                wifi_portal_close();
                play_sound(SND_UI);
                enter_view(VIEW_SET);
            }
            break;
        }
        return;
    }

    if (ev != BSP_BTN_LONG) return;

    if (s_view == VIEW_HOME && btn == BSP_BTN_OK) {
        s_set_cursor_row = 0;
        settings_refresh();
        enter_view(VIEW_SET);
        play_sound(SND_UI);
    } else if (s_view == VIEW_SET && btn == BSP_BTN_OK) {
        if (s_set_dirty) {
            profile_save(&s_profile);
            s_set_dirty = false;
        }
        play_sound(SND_UI);
        enter_view(VIEW_HOME);
    } else if (s_view == VIEW_PET && btn == BSP_BTN_OK && s_summoned) {
        if (pet_toggle_form(&s_pet)) {
            s_need_save = true;
            draw_pet();
            refresh_pet_scene();
            play_sound(SND_EVOLVE);
            show_toast(VIEW_PET, s_pet.form == PET_FORM_HUMAN ? "HUMAN FORM" : "BEAST FORM",
                       1500);
        } else {
            deny("HUMAN NEEDS 12 GROWTH");
        }
    }
}

// ---------------------------------------------------------------------------
// 电量
// ---------------------------------------------------------------------------
static void update_batt_label(lv_obj_t *label, uint32_t muted, uint32_t danger) {
    if (!label) return;
    if (s_hud_soc < 0) {
        lv_label_set_text(label, "--%");
        lv_obj_set_style_text_color(label, lv_color_hex(muted), 0);
    } else {
        char b[12];
        snprintf(b, sizeof(b), "%u%%", (unsigned)s_hud_soc);
        lv_label_set_text(label, b);
        lv_obj_set_style_text_color(label,
                                    lv_color_hex(s_hud_soc <= 15 ? danger : muted), 0);
    }
}

static void sample_battery(void) {
    s_soc = bsp_battery_soc();
    const int mv = bsp_battery_mv();
    const int64_t now = esp_timer_get_time();
    if (mv > 0) {
        s_batt_mv_avg = (s_batt_mv_avg < 0) ? mv : (s_batt_mv_avg * 7 + mv) / 8;
        if (s_batt_mv_ref < 0) {
            s_batt_mv_ref = s_batt_mv_avg;
            s_batt_ref_us = now;
        } else if (now - s_batt_ref_us >= 90000000LL) {
            // CW2017 无充电状态位：用静置电压趋势推断充电。
            s_charging = (s_soc >= 0 && s_soc < 100) &&
                         (s_batt_mv_avg - s_batt_mv_ref >= 30);
            s_batt_mv_ref = s_batt_mv_avg;
            s_batt_ref_us = now;
        }
    }
    if (s_soc == s_hud_soc) return;
    s_hud_soc = s_soc;
    update_batt_label(s_batt, T_MUTED, 0x8C1A12);
    update_batt_label(s_pet_batt, P_MUTED, P_DANGER);
    if (s_bolt) {
        if (s_charging) {
            lv_obj_remove_flag(s_bolt, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_bolt, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ---------------------------------------------------------------------------
// 音频合成
// ---------------------------------------------------------------------------
static void synth_sound(uint32_t id) {
    int dur_ms = 100;
    int f0 = 800;
    int f1 = 400;
    int amp = 3600;

    switch (id) {
    case SND_SUMMON: dur_ms = 420; f0 = 440; f1 = 990; amp = 4600; break;
    case SND_PET: dur_ms = 100; f0 = 880; f1 = 1180; amp = 3200; break;
    case SND_FEED: dur_ms = 160; f0 = 520; f1 = 760; amp = 3600; break;
    case SND_EVOLVE: dur_ms = 560; f0 = 520; f1 = 1320; amp = 5200; break;
    case SND_DENY: dur_ms = 180; f0 = 320; f1 = 210; amp = 3400; break;
    case SND_UI: dur_ms = 60; f0 = 880; f1 = 1040; amp = 2800; break;
    default: return;
    }

    const int total = 16000 * dur_ms / 1000;
    int16_t buf[256];
    uint32_t seed = 0x2545F491u ^ id;
    int phase = 0;

    for (int done = 0; done < total;) {
        int n = total - done;
        if (n > (int)(sizeof(buf) / sizeof(buf[0]))) n = (int)(sizeof(buf) / sizeof(buf[0]));
        for (int i = 0; i < n; i++) {
            const int idx = done + i;
            const int env = total - idx;
            const int freq = f0 + (f1 - f0) * idx / total;
            phase += freq;
            if (phase >= 16000) phase -= 16000;
            const int v = (phase < 8000) ? amp : -amp;
            buf[i] = (int16_t)(v * env / total);
        }
        if (bsp_audio_write(buf, (size_t)n * sizeof(int16_t)) != ESP_OK) break;
        done += n;
    }
    // 防止未使用告警（seed 保留给将来噪声音效）。
    (void)seed;
}

static void audio_task(void *arg) {
    (void)arg;
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "音频初始化失败，静音运行");
        vTaskDelete(NULL);
        return;
    }
    if (bsp_audio_set_format(16000, 16, 1) != ESP_OK) {
        ESP_LOGW(TAG, "音频格式设置失败，静音运行");
        vTaskDelete(NULL);
        return;
    }
    bsp_audio_set_volume(s_profile.volume);
    for (;;) {
        uint32_t id = 0;
        if (xQueueReceive(s_snd_q, &id, portMAX_DELAY) == pdTRUE) synth_sound(id);
    }
}

// ---------------------------------------------------------------------------
// 界面构建
// ---------------------------------------------------------------------------
static void build_home(void) {
    s_home_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_home_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_home_scr, 0, 0);
    lv_obj_set_style_border_width(s_home_scr, 0, 0);
    lv_obj_set_style_bg_color(s_home_scr, lv_color_hex(T_BG), 0);
    lv_obj_set_style_bg_grad_color(s_home_scr, lv_color_hex(T_BG2), 0);
    lv_obj_set_style_bg_grad_dir(s_home_scr, LV_GRAD_DIR_VER, 0);

    // 官方模板装饰条：顶左 72px 橙、顶右 72px 绿、底部全宽绿、y=26 分隔线。
    mk_box(s_home_scr, 0, 0, 72, 8, T_ACCENT, 0);
    mk_box(s_home_scr, 168, 0, 72, 8, T_DECO, 0);
    mk_box(s_home_scr, 0, 312, 240, 8, T_DECO, 0);
    mk_box(s_home_scr, 0, 26, 240, 2, T_LINE, 0);

    s_top_label = mk_text(s_home_scr, "SPROUT", &lv_font_montserrat_14, T_INK);
    lv_obj_set_pos(s_top_label, 8, 8);

    s_batt = mk_text(s_home_scr, "--%", &lv_font_montserrat_14, T_MUTED);
    lv_obj_align(s_batt, LV_ALIGN_TOP_RIGHT, -14, 7);

    static const lv_point_precise_t BOLT_PTS[] = {
        {4, 0}, {0, 7}, {3, 7}, {1, 13}, {8, 5}, {5, 5},
    };
    s_bolt = lv_line_create(s_home_scr);
    lv_line_set_points(s_bolt, BOLT_PTS, 6);
    lv_obj_set_style_line_width(s_bolt, 2, 0);
    lv_obj_set_style_line_color(s_bolt, lv_color_hex(T_INK), 0);
    lv_obj_set_pos(s_bolt, 184, 7);
    lv_obj_add_flag(s_bolt, LV_OBJ_FLAG_HIDDEN);

    // 身份卡：奶白底 + 3px 墨色描边 + 直角（官方 PIXEL 模板）。
    lv_obj_t *card = mk_box(s_home_scr, 12, 36, 216, 248, T_CARD, 0);
    lv_obj_set_style_border_width(card, 3, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(T_LINE), 0);
    lv_obj_set_style_shadow_width(card, 14, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_30, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(T_LINE), 0);

    // 头像框（64x64 + 3px 描边）
    s_avatar = mk_box(card, 14, 12, 64, 64, T_BG2, 0);
    lv_obj_set_style_border_width(s_avatar, 3, 0);
    lv_obj_set_style_border_color(s_avatar, lv_color_hex(T_LINE), 0);

    s_nick = mk_text(card, "ACE", &lv_font_montserrat_20, T_INK);
    lv_obj_set_pos(s_nick, 90, 10);
    lv_obj_set_width(s_nick, 112);
    lv_label_set_long_mode(s_nick, LV_LABEL_LONG_MODE_DOTS);

    s_sign = mk_text(card, "", &lv_font_montserrat_14, T_MUTED);
    lv_obj_set_pos(s_sign, 90, 38);
    lv_obj_set_width(s_sign, 112);
    lv_label_set_long_mode(s_sign, LV_LABEL_LONG_MODE_DOTS);

    s_status = mk_text(card, "READY", &lv_font_montserrat_14, T_BEST);
    lv_obj_set_pos(s_status, 90, 60);

    mk_box(card, 14, 86, 188, 2, 0xC7D8E4, 0);

    s_lv = mk_text(card, "LV 1", &lv_font_montserrat_20, T_INK);
    lv_obj_set_pos(s_lv, 14, 96);
    s_stage = mk_text(card, "SPROUT", &lv_font_montserrat_14, T_BEST);
    lv_obj_align(s_stage, LV_ALIGN_TOP_RIGHT, -14, 104);

    lv_obj_t *track = mk_box(card, 14, 126, 188, 8, T_TRACK, 4);
    s_grow_fill = mk_box(track, 0, 0, 0, 8, T_ACCENT, 4);
    s_grow_label = mk_text(card, "GROWTH 0", &lv_font_montserrat_14, T_MUTED);
    lv_obj_set_pos(s_grow_label, 14, 140);

    s_form_chip = mk_box(card, 14, 166, 92, 20, 0xFFE1B5, 10);
    lv_obj_set_style_border_width(s_form_chip, 2, 0);
    lv_obj_set_style_border_color(s_form_chip, lv_color_hex(T_BEST), 0);
    s_form_text = mk_text(s_form_chip, "BEAST", &lv_font_montserrat_14, T_INK);
    lv_obj_center(s_form_text);

    s_style_chip = mk_box(card, 110, 166, 92, 20, 0xFFE1B5, 10);
    lv_obj_set_style_border_width(s_style_chip, 2, 0);
    lv_obj_set_style_border_color(s_style_chip, lv_color_hex(T_BEST), 0);
    s_style_text = mk_text(s_style_chip, "CHIBI", &lv_font_montserrat_14, T_INK);
    lv_obj_center(s_style_text);

    for (int i = 0; i < 4; i++) {
        s_dots[i] = mk_box(card, 14 + i * 26, 198, 12, 12, T_ACCENT, 6);
    }

    lv_obj_t *btn = mk_box(card, 33, 214, 150, 28, T_ACCENT, 0);
    lv_obj_t *btn_text = mk_text(btn, "SUMMON", &lv_font_montserrat_20, T_ON_ACCENT);
    lv_obj_center(btn_text);

    lv_obj_t *hint = mk_text(s_home_scr, "OK SUMMON      HOLD OK SETTINGS",
                             &lv_font_montserrat_14, T_INK);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 296);
}

static void build_pet_scene(void) {
    s_pet_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_pet_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_pet_scr, 0, 0);
    lv_obj_set_style_border_width(s_pet_scr, 0, 0);
    lv_obj_set_style_bg_color(s_pet_scr, lv_color_hex(P_BG), 0);
    lv_obj_set_style_bg_grad_color(s_pet_scr, lv_color_hex(P_BG2), 0);
    lv_obj_set_style_bg_grad_dir(s_pet_scr, LV_GRAD_DIR_VER, 0);

    s_pet_title = mk_text(s_pet_scr, "LV 1", &lv_font_montserrat_14, P_ACCENT);
    lv_obj_set_pos(s_pet_title, 8, 8);
    s_pet_batt = mk_text(s_pet_scr, "--%", &lv_font_montserrat_14, P_MUTED);
    lv_obj_align(s_pet_batt, LV_ALIGN_TOP_RIGHT, -12, 8);

    s_pet_root = mk_clear(s_pet_scr, 60, 30, 120, 160);
    s_glow = mk_ring(s_pet_root, 39, 58, 42, 36, P_ACCENT, 2);
    lv_obj_add_flag(s_glow, LV_OBJ_FLAG_HIDDEN);
    s_art = mk_clear(s_pet_root, 0, 0, 120, 160);
    for (int i = 0; i < SPARKS; i++) {
        s_spark[i] = mk_box(s_pet_root, 58, 72, 4, 4, P_ACCENT, 2);
        lv_obj_add_flag(s_spark[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_react = mk_text(s_pet_root, "+MOOD", &lv_font_montserrat_14, P_WARM);
    lv_obj_set_pos(s_react, 42, 68);
    lv_obj_add_flag(s_react, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *mood = mk_text(s_pet_scr, "MOOD", &lv_font_montserrat_14, P_MUTED);
    lv_obj_set_pos(mood, 24, 200);
    lv_obj_t *mt = mk_box(s_pet_scr, 78, 204, 138, 8, P_TRACK, 4);
    s_mood_fill = mk_box(mt, 0, 0, 0, 8, P_ACCENT, 4);

    lv_obj_t *full = mk_text(s_pet_scr, "FULL", &lv_font_montserrat_14, P_MUTED);
    lv_obj_set_pos(full, 24, 222);
    lv_obj_t *ft = mk_box(s_pet_scr, 78, 226, 138, 8, P_TRACK, 4);
    s_full_fill = mk_box(ft, 0, 0, 0, 8, P_WARM, 4);

    s_pet_hint = mk_text(s_pet_scr, "UP PET    DOWN FEED", &lv_font_montserrat_14, P_INK);
    lv_obj_align(s_pet_hint, LV_ALIGN_TOP_MID, 0, 250);
    s_pet_sub = mk_text(s_pet_scr, "OK TUCK", &lv_font_montserrat_14, P_MUTED);
    lv_obj_align(s_pet_sub, LV_ALIGN_TOP_MID, 0, 268);
    s_pet_status = mk_text(s_pet_scr, "READY", &lv_font_montserrat_14, P_ACCENT);
    lv_obj_align(s_pet_status, LV_ALIGN_TOP_MID, 0, 288);

    s_pet_toast_box = mk_box(s_pet_scr, 20, 150, 200, 30, 0x163A34, 15);
    lv_obj_set_style_border_width(s_pet_toast_box, 1, 0);
    lv_obj_set_style_border_color(s_pet_toast_box, lv_color_hex(P_ACCENT), 0);
    s_pet_toast = mk_text(s_pet_toast_box, "", &lv_font_montserrat_14, P_INK);
    lv_obj_center(s_pet_toast);
    lv_obj_add_flag(s_pet_toast_box, LV_OBJ_FLAG_HIDDEN);

    s_flash = mk_box(s_pet_scr, 0, 0, SCREEN_W, SCREEN_H, 0xFFFFFF, 0);
    lv_obj_set_style_opa(s_flash, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_flash, LV_OBJ_FLAG_HIDDEN);

    draw_pet();
}

static void build_settings(void) {
    s_set_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_set_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_set_scr, 0, 0);
    lv_obj_set_style_border_width(s_set_scr, 0, 0);
    lv_obj_set_style_bg_color(s_set_scr, lv_color_hex(T_BG), 0);
    lv_obj_set_style_bg_grad_color(s_set_scr, lv_color_hex(T_BG2), 0);
    lv_obj_set_style_bg_grad_dir(s_set_scr, LV_GRAD_DIR_VER, 0);

    mk_box(s_set_scr, 0, 0, 72, 8, T_ACCENT, 0);
    mk_box(s_set_scr, 168, 0, 72, 8, T_DECO, 0);
    mk_box(s_set_scr, 0, 312, 240, 8, T_DECO, 0);

    lv_obj_t *title = mk_text(s_set_scr, "SETTINGS", &lv_font_montserrat_14, T_MUTED);
    lv_obj_set_pos(title, 8, 12);

    for (int i = 0; i < SET_ROWS; i++) {
        const int y = 44 + i * 44;
        s_set_cursor[i] = mk_box(s_set_scr, 8, y + 2, 4, 24, T_ACCENT, 2);
        s_set_name[i] = mk_text(s_set_scr, SET_NAMES[i], &lv_font_montserrat_14, T_MUTED);
        lv_obj_set_pos(s_set_name[i], 20, y + 4);
        s_set_val[i] = mk_text(s_set_scr, "", &lv_font_montserrat_20, T_INK);
        lv_obj_align(s_set_val[i], LV_ALIGN_TOP_RIGHT, -16, y);
    }

    s_set_toast_box = mk_box(s_set_scr, 20, 276, 200, 30, T_CARD, 15);
    lv_obj_set_style_border_width(s_set_toast_box, 2, 0);
    lv_obj_set_style_border_color(s_set_toast_box, lv_color_hex(T_BEST), 0);
    s_set_toast = mk_text(s_set_toast_box, "", &lv_font_montserrat_14, T_INK);
    lv_obj_center(s_set_toast);
    lv_obj_add_flag(s_set_toast_box, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *hint = mk_text(s_set_scr, "OK CHANGE      HOLD OK EXIT",
                             &lv_font_montserrat_14, T_INK);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 300);
}

static void build_portal(void) {
    s_portal_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_portal_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_portal_scr, 0, 0);
    lv_obj_set_style_border_width(s_portal_scr, 0, 0);
    lv_obj_set_style_bg_color(s_portal_scr, lv_color_hex(T_BG), 0);
    lv_obj_set_style_bg_grad_color(s_portal_scr, lv_color_hex(T_BG2), 0);
    lv_obj_set_style_bg_grad_dir(s_portal_scr, LV_GRAD_DIR_VER, 0);

    mk_box(s_portal_scr, 0, 0, 72, 8, T_ACCENT, 0);
    mk_box(s_portal_scr, 168, 0, 72, 8, T_DECO, 0);
    mk_box(s_portal_scr, 0, 312, 240, 8, T_DECO, 0);

    lv_obj_t *title = mk_text(s_portal_scr, "PHONE SETUP", &lv_font_montserrat_14, T_MUTED);
    lv_obj_set_pos(title, 8, 12);

    lv_obj_t *l1 = mk_text(s_portal_scr, "SSID", &lv_font_montserrat_14, T_MUTED);
    lv_obj_set_pos(l1, 16, 44);
    s_portal_ssid = mk_text(s_portal_scr, "", &lv_font_montserrat_20, T_INK);
    lv_obj_set_pos(s_portal_ssid, 16, 62);

    lv_obj_t *l2 = mk_text(s_portal_scr, "PASSWORD", &lv_font_montserrat_14, T_MUTED);
    lv_obj_set_pos(l2, 16, 98);
    s_portal_pass = mk_text(s_portal_scr, "", &lv_font_montserrat_20, T_INK);
    lv_obj_set_pos(s_portal_pass, 16, 116);

    lv_obj_t *l3 = mk_text(s_portal_scr, "OPEN IN BROWSER", &lv_font_montserrat_14, T_MUTED);
    lv_obj_set_pos(l3, 16, 152);
    lv_obj_t *url = mk_text(s_portal_scr, "http://192.168.4.1", &lv_font_montserrat_20,
                            T_BEST);
    lv_obj_set_pos(url, 16, 170);

    lv_obj_t *l4 = mk_text(s_portal_scr, "AUTO CLOSE", &lv_font_montserrat_14, T_MUTED);
    lv_obj_align(l4, LV_ALIGN_TOP_MID, 0, 214);
    s_portal_left = mk_text(s_portal_scr, "3:00", &lv_font_montserrat_20, T_BEST);
    lv_obj_align(s_portal_left, LV_ALIGN_TOP_MID, 0, 232);

    s_portal_toast_box = mk_box(s_portal_scr, 20, 262, 200, 28, T_CARD, 14);
    lv_obj_set_style_border_width(s_portal_toast_box, 2, 0);
    lv_obj_set_style_border_color(s_portal_toast_box, lv_color_hex(T_BEST), 0);
    s_portal_toast = mk_text(s_portal_toast_box, "", &lv_font_montserrat_14, T_INK);
    lv_obj_center(s_portal_toast);
    lv_obj_add_flag(s_portal_toast_box, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *hint = mk_text(s_portal_scr, "OK EXIT", &lv_font_montserrat_14, T_INK);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 298);
}

// ---------------------------------------------------------------------------
// 主循环
// ---------------------------------------------------------------------------
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_input_q) return;
    const input_ev_t e = {.btn = (uint8_t)btn, .ev = (uint8_t)ev};
    (void)xQueueSend(s_input_q, &e, 0);
}

static void wake_screen(void) {
    s_dimmed = false;
    bsp_display_backlight(s_profile.bright ? s_profile.bright : 100);
    s_last_act_us = esp_timer_get_time();
    if (s_view != VIEW_HOME) {
        if (s_view == VIEW_PORTAL) wifi_portal_close();
        s_summoned = false;
        s_anim = ANIM_NONE;
        enter_view(VIEW_HOME);
    }
}

static void app_task(void *arg) {
    (void)arg;
    TickType_t wake = xTaskGetTickCount();
    int64_t last_us = esp_timer_get_time();
    uint32_t frame = 0;

    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(TICK_MS));
        const int64_t now_us = esp_timer_get_time();
        uint32_t dt = (uint32_t)((now_us - last_us) / 1000);
        last_us = now_us;
        if (dt > 100) dt = 100;

        if (s_dimmed) {
            input_ev_t drop;
            bool woke = false;
            while (s_input_q && xQueueReceive(s_input_q, &drop, 0) == pdTRUE) woke = true;
            if (woke) wake_screen();
            continue;
        }

        if (bsp_lvgl_lock(200)) {
            input_ev_t in;
            while (s_input_q && xQueueReceive(s_input_q, &in, 0) == pdTRUE) {
                handle_key(in.btn, in.ev);
            }
            bsp_lvgl_unlock();
        }

        if (s_need_save) {
            s_need_save = false;
            (void)pet_store_save(&s_pet);
        }

        if (++frame % 30 == 0) sample_battery();

        // 门户开启时不息屏（3 分钟无请求由门户自己超时关闭）。
        const bool portal_open = (s_view == VIEW_PORTAL);
        const uint8_t idle_sec = s_profile.idle;
        if (!portal_open && idle_sec > 0 &&
            now_us - s_last_act_us >= (int64_t)idle_sec * 1000000LL) {
            s_dimmed = true;
            bsp_display_backlight(5);
            continue;
        }

        if (bsp_lvgl_lock(200)) {
            ui_tick(dt);
            bsp_lvgl_unlock();
        }
    }
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------
void pet_app_set_epoch(int64_t epoch) {
    if (epoch <= 0) return;
    s_epoch_base = epoch;
    s_epoch_anchor_us = esp_timer_get_time();
    s_time_known = true;
    ESP_LOGI(TAG, "NTP 校时: epoch=%lld", (long long)epoch);
    if (s_home_scr && bsp_lvgl_lock(200)) {
        refresh_status();
        bsp_lvgl_unlock();
    }
}

void pet_app_refresh(void) {
    const uint8_t old_pstyle = s_profile.pstyle;
    profile_load(&s_profile);
    bsp_audio_set_volume(s_profile.volume);
    if (!s_home_scr) return;
    if (bsp_lvgl_lock(200)) {
        if (old_pstyle != s_profile.pstyle) {
            draw_pet();
        }
        refresh_home();
        bsp_lvgl_unlock();
    }
}

void pet_app_start(void) {
    profile_load(&s_profile);

    pet_load_info_t info;
    pet_store_load(&s_pet, &info);
    s_last_act_us = esp_timer_get_time();

    s_input_q = xQueueCreate(16, sizeof(input_ev_t));
    s_snd_q = xQueueCreate(8, sizeof(uint32_t));
    if (!s_input_q || !s_snd_q) {
        ESP_LOGE(TAG, "输入/音频队列创建失败");
        return;
    }

    if (bsp_lvgl_lock(1000)) {
        build_home();
        build_pet_scene();
        build_settings();
        build_portal();
        enter_view(VIEW_HOME);
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        ESP_LOGI(TAG, "LVGL 内存: 已用 %u / %u B (%u%%), 最大空闲块 %u B, 碎片 %u%%",
                 (unsigned)(mon.total_size - mon.free_size), (unsigned)mon.total_size,
                 (unsigned)mon.used_pct, (unsigned)mon.free_biggest_size,
                 (unsigned)mon.frag_pct);
        bsp_lvgl_unlock();
    } else {
        ESP_LOGE(TAG, "LVGL 锁定超时，无法创建界面");
        return;
    }

    bsp_display_backlight(s_profile.bright ? s_profile.bright : 100);

    const esp_err_t btn = bsp_button_init(on_key, NULL);
    if (btn != ESP_OK) ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(btn));

    if (xTaskCreate(audio_task, "pet_audio", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "音频任务创建失败，静音运行");
    }
    if (xTaskCreate(app_task, "pet_app", 6144, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "主循环任务创建失败");
    }

    if (!info.had_valid) (void)pet_store_save(&s_pet);

    ESP_LOGI(TAG, "宠物养成启动: growth=%lu stage=%u form=%u pstyle=%u",
             (unsigned long)s_pet.growth, (unsigned)s_pet.stage, (unsigned)s_pet.form,
             (unsigned)s_profile.pstyle);
}
