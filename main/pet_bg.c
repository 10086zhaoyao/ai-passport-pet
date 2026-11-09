// main/pet_bg.c
// 召唤场景背景：天空渐变 + 太阳/月亮/星/云/雨/雪/远山/地面。
// 设计约束：
//  - 所有对象开机创建一次，切换天气/时段只改颜色与透明度，避免重建导致的闪屏；
//  - 过渡 700ms（颜色）与 450ms（元素透明度），30FPS 下约 21/14 帧；
//  - 未授时（epoch < 0）固定使用默认深青背景，不显示任何天气元素。
#include "pet_bg.h"

#include "esp_timer.h"

#define SKY_W 240
#define SKY_H 196
#define RAIN_N 8
#define SNOW_N 6
#define STAR_N 6

enum {
    SKY_DEFAULT = 0,
    SKY_DAWN,
    SKY_DAY,
    SKY_DUSK,
    SKY_NIGHT,
    SKY_COUNT,
};

enum {
    GRP_SUN = 0,
    GRP_MOON,
    GRP_STARS,
    GRP_CLOUDS,
    GRP_RAIN,
    GRP_SNOW,
    GRP_COUNT,
};

typedef struct {
    uint32_t top;
    uint32_t bottom;
    uint32_t hill;
    uint32_t ground;
} sky_pal_t;

static const sky_pal_t PAL[SKY_COUNT] = {
    // 默认：沿用原召唤场景的深青玉，离线/未授时使用。
    {0x071319, 0x0B2326, 0x0E2A2A, 0x0A1D1F},
    // 黎明：冷蓝带一点暖灰。
    {0x2B4350, 0x18353B, 0x1B3B3C, 0x0E2628},
    // 白天：低饱和青绿，保证卡片下方文字对比度。
    {0x2C6E78, 0x17494F, 0x1B4A46, 0x0E2E2F},
    // 黄昏：灰紫压暗。
    {0x5C3A4E, 0x21303A, 0x2A3138, 0x121F24},
    // 夜晚：接近默认，偏冷。
    {0x071319, 0x0B2326, 0x0E2A2A, 0x0A1D1F},
};

static lv_obj_t *s_sky;
static lv_obj_t *s_grp[GRP_COUNT];
static lv_obj_t *s_hill[2];
static lv_obj_t *s_ground;
static lv_obj_t *s_rain[RAIN_N];
static lv_obj_t *s_snow[SNOW_N];
static lv_obj_t *s_sun;
static lv_obj_t *s_moon;

static int s_bucket = -1;
static wx_kind_t s_wx = WX_UNKNOWN;

static lv_color_t s_from_top, s_from_bottom, s_from_hill, s_from_ground;
static lv_color_t s_to_top, s_to_bottom, s_to_hill, s_to_ground;
static lv_color_t s_now_top, s_now_bottom, s_now_hill, s_now_ground;
static uint16_t s_mix;  // 0..255，255 = 完全到达目标

static uint8_t s_opa[GRP_COUNT];
static uint8_t s_opa_target[GRP_COUNT];

static struct {
    int16_t x;
    int16_t y;
    uint8_t spd;
} s_rain_pos[RAIN_N];

static struct {
    int16_t x;
    int16_t y;
    uint8_t spd;
} s_snow_pos[SNOW_N];

static uint32_t s_ms;

// ---------------------------------------------------------------------------
// 基础绘制
// ---------------------------------------------------------------------------
static lv_obj_t *bg_clear(lv_obj_t *parent, int x, int y, int w, int h) {
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

static lv_obj_t *bg_box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color,
                        int radius) {
    lv_obj_t *o = bg_clear(parent, x, y, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    return o;
}

static lv_obj_t *bg_ring(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color,
                         int width) {
    lv_obj_t *o = bg_clear(parent, x, y, w, h);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(o, width, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(color), 0);
    return o;
}

static uint32_t mix_hex(uint32_t a, uint32_t b, int pct) {
    const lv_color_t ca = lv_color_hex(a);
    const lv_color_t cb = lv_color_hex(b);
    const lv_color_t m = lv_color_mix(cb, ca, (uint8_t)(255 * pct / 100));
    return lv_color_to_u32(m);
}

// ---------------------------------------------------------------------------
// 时段与天气目标
// ---------------------------------------------------------------------------
static int bucket_of(int64_t epoch) {
    if (epoch < 0) return SKY_DEFAULT;
    int64_t local = epoch + 8 * 3600;  // 产品面向国内，固定 UTC+8
    const int h = (int)((local / 3600) % 24);
    if (h >= 5 && h < 7) return SKY_DAWN;
    if (h >= 7 && h < 17) return SKY_DAY;
    if (h >= 17 && h < 19) return SKY_DUSK;
    return SKY_NIGHT;
}

static int weather_tint_pct(wx_kind_t wx) {
    switch (wx) {
    case WX_CLOUDY: return 18;
    case WX_OVERCAST: return 32;
    case WX_RAIN: return 32;
    case WX_SNOW: return 20;
    case WX_FOG: return 25;
    case WX_THUNDER: return 36;
    default: return 0;
    }
}

static uint32_t weather_tint_color(wx_kind_t wx) {
    switch (wx) {
    case WX_SNOW: return 0xB9D2DA;
    case WX_THUNDER: return 0x1D2B3A;
    case WX_RAIN: return 0x37505E;
    case WX_OVERCAST: return 0x5D6A70;
    case WX_FOG: return 0x6E7C80;
    case WX_CLOUDY: return 0x6B7C82;
    default: return 0;
    }
}

static uint8_t target_sun(int bucket, wx_kind_t wx) {
    if (bucket == SKY_DEFAULT || bucket == SKY_NIGHT) return 0;
    if (wx == WX_OVERCAST || wx == WX_RAIN || wx == WX_THUNDER || wx == WX_SNOW ||
        wx == WX_FOG) {
        return 40;
    }
    if (wx == WX_CLOUDY) return 110;
    return bucket == SKY_DUSK ? 235 : 205;
}

static uint8_t target_moon(int bucket, wx_kind_t wx) {
    if (bucket != SKY_NIGHT) return 0;
    switch (wx) {
    case WX_SUNNY: return 230;
    case WX_CLOUDY: return 90;
    case WX_UNKNOWN: return 200;
    case WX_FOG: return 40;
    default: return 30;
    }
}

static uint8_t target_stars(int bucket, wx_kind_t wx) {
    if (bucket == SKY_DAWN) return 24;
    if (bucket != SKY_NIGHT) return 0;
    switch (wx) {
    case WX_SUNNY: return 220;
    case WX_CLOUDY: return 70;
    case WX_UNKNOWN: return 180;
    case WX_FOG: return 30;
    default: return 0;
    }
}

static uint8_t target_clouds(int bucket, wx_kind_t wx) {
    if (bucket == SKY_DEFAULT) return 0;
    switch (wx) {
    case WX_SUNNY: return 55;
    case WX_CLOUDY: return 165;
    case WX_OVERCAST: return 225;
    case WX_RAIN: return 215;
    case WX_SNOW: return 185;
    case WX_FOG: return 235;
    case WX_THUNDER: return 240;
    default: return 0;
    }
}

static void set_targets(void) {
    const int tint = weather_tint_pct(s_wx);
    const uint32_t tc = weather_tint_color(s_wx);
    const sky_pal_t *p = &PAL[s_bucket];

    s_from_top = s_now_top;
    s_from_bottom = s_now_bottom;
    s_from_hill = s_now_hill;
    s_from_ground = s_now_ground;

    s_to_top = (tint > 0) ? lv_color_hex(mix_hex(p->top, tc, tint))
                          : lv_color_hex(p->top);
    s_to_bottom = (tint > 0) ? lv_color_hex(mix_hex(p->bottom, tc, tint))
                             : lv_color_hex(p->bottom);
    s_to_hill = (tint > 0) ? lv_color_hex(mix_hex(p->hill, tc, tint))
                           : lv_color_hex(p->hill);
    s_to_ground = (tint > 0) ? lv_color_hex(mix_hex(p->ground, tc, tint))
                             : lv_color_hex(p->ground);
    s_mix = 0;

    s_opa_target[GRP_SUN] = target_sun(s_bucket, s_wx);
    s_opa_target[GRP_MOON] = target_moon(s_bucket, s_wx);
    s_opa_target[GRP_STARS] = target_stars(s_bucket, s_wx);
    s_opa_target[GRP_CLOUDS] = target_clouds(s_bucket, s_wx);
    s_opa_target[GRP_RAIN] =
        (s_wx == WX_RAIN) ? 220 : (s_wx == WX_THUNDER ? 255 : 0);
    s_opa_target[GRP_SNOW] = (s_wx == WX_SNOW) ? 225 : 0;
}

// ---------------------------------------------------------------------------
// 创建
// ---------------------------------------------------------------------------
void pet_bg_create(lv_obj_t *parent) {
    s_sky = lv_obj_create(parent);
    lv_obj_remove_flag(s_sky, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_sky, 0, 0);
    lv_obj_set_size(s_sky, SKY_W, SKY_H);
    lv_obj_set_style_radius(s_sky, 0, 0);
    lv_obj_set_style_border_width(s_sky, 0, 0);
    lv_obj_set_style_pad_all(s_sky, 0, 0);
    lv_obj_set_style_bg_opa(s_sky, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(s_sky, LV_GRAD_DIR_VER, 0);

    s_now_top = lv_color_hex(PAL[SKY_DEFAULT].top);
    s_now_bottom = lv_color_hex(PAL[SKY_DEFAULT].bottom);
    s_now_hill = lv_color_hex(PAL[SKY_DEFAULT].hill);
    s_now_ground = lv_color_hex(PAL[SKY_DEFAULT].ground);
    lv_obj_set_style_bg_color(s_sky, s_now_top, 0);
    lv_obj_set_style_bg_grad_color(s_sky, s_now_bottom, 0);

    for (int i = 0; i < GRP_COUNT; i++) {
        s_grp[i] = bg_clear(s_sky, 0, 0, SKY_W, SKY_H);
        s_opa[i] = 0;
        s_opa_target[i] = 0;
        lv_obj_set_style_opa(s_grp[i], LV_OPA_TRANSP, 0);
    }

    // 星
    static const int16_t STAR_XY[STAR_N][2] = {
        {26, 38}, {58, 24}, {94, 46}, {128, 28}, {150, 66}, {42, 74},
    };
    for (int i = 0; i < STAR_N; i++) {
        bg_box(s_grp[GRP_STARS], STAR_XY[i][0], STAR_XY[i][1], (i % 2) ? 3 : 4,
               (i % 2) ? 3 : 4, 0xEAF6F2, LV_RADIUS_CIRCLE);
    }

    // 太阳：外圈光晕 + 实心圆（放在星之后、云之前）
    lv_obj_set_pos(s_grp[GRP_SUN], 156, 26);
    lv_obj_set_size(s_grp[GRP_SUN], 66, 66);
    bg_ring(s_grp[GRP_SUN], 6, 6, 54, 54, 0xFFDDA0, 2);
    s_sun = bg_box(s_grp[GRP_SUN], 16, 16, 34, 34, 0xFFE2A0, LV_RADIUS_CIRCLE);

    // 月亮：实心圆 + 三个环形山
    lv_obj_set_pos(s_grp[GRP_MOON], 160, 36);
    lv_obj_set_size(s_grp[GRP_MOON], 46, 46);
    s_moon = bg_box(s_grp[GRP_MOON], 8, 6, 28, 28, 0xEAF6F2, LV_RADIUS_CIRCLE);
    bg_box(s_grp[GRP_MOON], 14, 12, 6, 6, 0xC9DCD8, LV_RADIUS_CIRCLE);
    bg_box(s_grp[GRP_MOON], 24, 19, 5, 5, 0xC9DCD8, LV_RADIUS_CIRCLE);
    bg_box(s_grp[GRP_MOON], 16, 24, 4, 4, 0xC9DCD8, LV_RADIUS_CIRCLE);

    // 云：三簇，低对比的雾白
    static const int16_t CLOUD[9][4] = {
        {12, 58, 46, 14}, {28, 50, 36, 14}, {50, 58, 32, 12},
        {94, 40, 42, 13}, {110, 33, 30, 12}, {124, 41, 26, 11},
        {164, 78, 44, 12}, {178, 71, 28, 11}, {154, 84, 26, 9},
    };
    for (int i = 0; i < 9; i++) {
        bg_box(s_grp[GRP_CLOUDS], CLOUD[i][0], CLOUD[i][1], CLOUD[i][2], CLOUD[i][3],
               0xE6F4F4, 7);
    }

    // 远山 + 地面（在云之后创建，压住云脚）
    s_hill[0] = bg_box(s_sky, -26, 146, 156, 62, PAL[SKY_DEFAULT].hill, 40);
    s_hill[1] = bg_box(s_sky, 132, 154, 134, 54, PAL[SKY_DEFAULT].hill, 36);
    lv_obj_set_style_bg_color(s_hill[1], lv_color_hex(mix_hex(PAL[SKY_DEFAULT].hill,
                                                             0x000000, 22)),
                              0);
    s_ground = bg_box(s_sky, 0, 184, SKY_W, 12, PAL[SKY_DEFAULT].ground, 0);

    // 雨丝
    for (int i = 0; i < RAIN_N; i++) {
        s_rain_pos[i].x = (int16_t)(10 + i * 29);
        s_rain_pos[i].y = (int16_t)((i * 37) % (SKY_H - 12));
        s_rain_pos[i].spd = (uint8_t)(205 + (i % 4) * 38);
        s_rain[i] = bg_box(s_grp[GRP_RAIN], s_rain_pos[i].x, s_rain_pos[i].y, 2, 11,
                           0xBFE3F2, 1);
    }

    // 雪点
    for (int i = 0; i < SNOW_N; i++) {
        s_snow_pos[i].x = (int16_t)(18 + i * 37);
        s_snow_pos[i].y = (int16_t)((i * 61) % (SKY_H - 8));
        s_snow_pos[i].spd = (uint8_t)(20 + i * 5);
        s_snow[i] = bg_box(s_grp[GRP_SNOW], s_snow_pos[i].x, s_snow_pos[i].y, 5, 5,
                           0xEAF6F2, LV_RADIUS_CIRCLE);
    }

    // 雨雪分组放到最上层（创建顺序已在云之后，但容器占位在前；此处提升）。
    lv_obj_move_foreground(s_grp[GRP_RAIN]);
    lv_obj_move_foreground(s_grp[GRP_SNOW]);

    (void)s_sun;
    (void)s_moon;
}

// ---------------------------------------------------------------------------
// 每帧
// ---------------------------------------------------------------------------
void pet_bg_tick(uint32_t dt_ms, int64_t epoch, wx_kind_t weather) {
    if (!s_sky) return;

    const int bucket = bucket_of(epoch);
    const wx_kind_t wx = (bucket == SKY_DEFAULT) ? WX_UNKNOWN : weather;
    if (bucket != s_bucket || wx != s_wx) {
        s_bucket = bucket;
        s_wx = wx;
        set_targets();
    }

    s_ms += dt_ms;

    if (s_mix < 255) {
        int step = (int)(dt_ms * 255 / 700);
        if (step < 1) step = 1;
        s_mix = (uint16_t)((s_mix + step > 255) ? 255 : s_mix + step);
    }
    s_now_top = lv_color_mix(s_to_top, s_from_top, (uint8_t)s_mix);
    s_now_bottom = lv_color_mix(s_to_bottom, s_from_bottom, (uint8_t)s_mix);
    s_now_hill = lv_color_mix(s_to_hill, s_from_hill, (uint8_t)s_mix);
    s_now_ground = lv_color_mix(s_to_ground, s_from_ground, (uint8_t)s_mix);
    lv_obj_set_style_bg_color(s_sky, s_now_top, 0);
    lv_obj_set_style_bg_grad_color(s_sky, s_now_bottom, 0);
    lv_obj_set_style_bg_color(s_hill[0], s_now_hill, 0);
    lv_obj_set_style_bg_color(s_hill[1],
                              lv_color_mix(lv_color_hex(0x000000), s_now_hill, 56), 0);
    lv_obj_set_style_bg_color(s_ground, s_now_ground, 0);

    // 元素透明度渐变
    int opa_step = (int)(dt_ms * 255 / 450);
    if (opa_step < 2) opa_step = 2;
    for (int i = 0; i < GRP_COUNT; i++) {
        const int target = s_opa_target[i];
        if (s_opa[i] == target) continue;
        int v = s_opa[i];
        if (v < target) {
            v = (v + opa_step > target) ? target : v + opa_step;
        } else {
            v = (v - opa_step < target) ? target : v - opa_step;
        }
        s_opa[i] = (uint8_t)v;
        lv_obj_set_style_opa(s_grp[i], (lv_opa_t)v, 0);
    }

    // 雨：竖直下落，出界回卷。
    if (s_opa[GRP_RAIN] > 0) {
        for (int i = 0; i < RAIN_N; i++) {
            int y = s_rain_pos[i].y + (int)(s_rain_pos[i].spd * dt_ms / 1000);
            if (y > SKY_H - 4) y = -12;
            s_rain_pos[i].y = (int16_t)y;
            lv_obj_set_pos(s_rain[i], s_rain_pos[i].x, y);
        }
    }

    // 雪：慢速下落 + 三角波横向漂移。
    if (s_opa[GRP_SNOW] > 0) {
        static const int8_t SWAY[8] = {0, 1, 2, 3, 2, 1, 0, -1};
        for (int i = 0; i < SNOW_N; i++) {
            int y = s_snow_pos[i].y + (int)(s_snow_pos[i].spd * dt_ms / 1000);
            if (y > SKY_H - 3) y = -8;
            s_snow_pos[i].y = (int16_t)y;
            const int sway = SWAY[((s_ms / 180) + (uint32_t)i) % 8] * 2;
            lv_obj_set_pos(s_snow[i], s_snow_pos[i].x + sway, y);
        }
    }
}
