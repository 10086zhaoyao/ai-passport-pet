// main/pet_app.c
// 电子身份卡 · 宠物养成。
// 首页只做身份牌本身；宠物按 OK 召唤，退出即收纳。时间基准：NTP 墙钟（UTC），
// 未校时前互动锁定。全部界面共用 ui_theme.h 的一套视觉规范。
#include "pet_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fonts.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "nvs.h"
#include "pet_bg.h"
#include "pet_logic.h"
#include "pet_store.h"
#include "profile_store.h"
#include "screen_capture.h"
#include "sprites_info.h"
#include "themes_info.h"
#include "avatar_default.h"
#include "ui_theme.h"
#include "weather.h"
#include "wifi_portal.h"

static const char *TAG = "pet_app";

#define TICK_MS 33
#define SCREEN_W 240
#define SCREEN_H 320
#define SPARKS 6
#define SET_ROWS 8
// 配网入口与语言行必须用命名索引：新增第 8 行后 SET_ROWS-1 已不再是 PHONE SETUP。
#define SET_ROW_PORTAL 6
#define SET_ROW_LANG 7
// 界面语言开关：只切界面文案；宠物名、昵称、签名和 80 条气泡始终按原文显示。
#define TR(zh, en) ((s_profile.lang) ? (en) : (zh))
#define BRIGHT_MIN 10
#define AV_W 104
#define AV_H 104

// 统一视觉规范见 ui_theme.h：深石墨机身 + 暖纸卡面 + 安全橙点色。
// 设置/门户页沿用同一套色板，避免与首页出现色调跳变。
#define T_BG U_BG
#define T_BG2 U_BG2
#define T_CARD U_PANEL
#define T_INK U_TEXT
#define T_ACCENT U_ACCENT
#define T_ON_ACCENT U_INK
#define T_BEST U_ACCENT
#define T_MUTED U_MUTED
#define T_DECO U_ACCENT
#define T_LINE U_LINE
#define T_TRACK U_TRACK

// 首页身份牌（纸面语境）。
#define H_BG U_BG
#define H_GRID U_LINE_SOFT
#define H_PAPER U_PAPER
#define H_INK U_INK
#define H_INK2 U_INK2
#define H_MUTED U_MUTED
#define H_LINE U_PAPER_LINE
#define H_ACCENT U_ACCENT
#define H_DANGER U_DANGER

// 召唤场景：同一机身的次层分区，点色统一为安全橙；暖光留给宠物本身。
#define P_BG U_BG
#define P_BG2 U_BG2
#define P_CARD U_PANEL
#define P_EDGE U_LINE
#define P_INK U_TEXT
#define P_MUTED U_MUTED
#define P_ACCENT U_ACCENT
#define P_WARM 0xFFC46B
#define P_DANGER U_DANGER
#define P_TRACK U_TRACK
#define P_INK_SOFT U_INK

enum {
    SND_SUMMON = 1,
    SND_PET,
    SND_FEED,
    SND_EVOLVE,
    SND_DENY,
    SND_UI,
    SND_CURIOUS,
    SND_KITTEN,
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

// 首页 = 纯身份牌：卡面本体 + STAGE 里的猫/头像 + 底部文字区。
static lv_obj_t *s_home_scr;
static lv_obj_t *s_theme_bg;    // 预渲染整卡底图（最底层，切主题只换源）
static lv_image_dsc_t s_theme_dsc;
static lv_obj_t *s_home_body;   // 卡面内容容器：仅用于 180ms 位移入场
static lv_obj_t *s_batt;
static lv_obj_t *s_bolt;
static lv_obj_t *s_clock;       // 左上时间（NTP 未校时显示 --:--）
static lv_obj_t *s_nick;
static lv_obj_t *s_sign;
static lv_obj_t *s_ttl;         // MICRO-L：AI PASSPORT
static lv_obj_t *s_rev;         // MICRO-R：NO. 01
static lv_obj_t *s_home_cat;    // STAGE 主视觉：猫
static lv_obj_t *s_home_acc_body;  // 首页配件：身体（西装 / 酷外套）
static lv_obj_t *s_home_acc_face;  // 首页配件：面部（酷墨镜）
static lv_obj_t *s_hero_box;    // STAGE 主视觉：头像容器（圆角裁剪）
static lv_obj_t *s_hero_img;
static lv_obj_t *s_mini_cat;    // 右下角：待机小猫（无圆框、无描边）
static lv_image_dsc_t s_av_dsc;
// 88x88 RGB565A8 = 23,232 B：上传源就是 88x88 原生，主视觉 2 倍（176）整数放大，
// 固件全程不插值。放静态 DRAM（.bss），不占 LVGL 池、也没有任何 lv_malloc 路径。
static uint8_t *s_av_store;   // 只有用户传过头像才申请（23,232 B 堆），门户期间释放
static uint8_t *s_av_buf;     // 当前 dsc 指向：堆缓冲，或 flash 里的内置头像
static bool s_av_heap;        // s_av_buf 是否指向堆
static bool s_av_released;    // 门户期间已释放，回首页时重建
static int32_t s_home_in_ms = -1;

// 场景页底部署名（拍立得落款），坐标与资源为美术终稿（240x320 绝对坐标）：
// 合影 68x34 @ (16,250) 1:1 贴、不加 padding、不缩放；名字基线 (88,284)，字距合影右缘 4px。
// 图里第 34 行仍有猫身像素、46 头像自带 1px 圆环，所以都不能再裁不能再缩。
#define SIGN_X 16
#define SIGN_Y 250
#define SIGN_IMG_W 68
#define SIGN_IMG_H 34
#define SIGN_NAME_X 88
#define SIGN_NAME_BASE_Y 284
#define SIGN_HINT_RIGHT 218
#define SIGN_HINT_BASE_Y 296
#define SIGN_KEY_L_X 20
#define SIGN_KEY_R_RIGHT 220
#define SIGN_KEY_Y 306
// 猫名取自 NVS 的 petname（配网页可改，上限 24 B）；未设过或为空时回落默认值。
// 界面切英文时这里也不翻译，始终按中文原样渲染。
// 署名文字色：美术给的深色底版本（当前场景页恒为深色面板）。
#define SIGN_INK 0xC8D0D4
static lv_obj_t *s_duo_box;
static lv_obj_t *s_duo_img;
static lv_obj_t *s_duo_text;

// 主视觉：0=猫（默认），1=头像。持久化在 profile.hero。
#define HERO_CAT 0
#define HERO_AVATAR 1

// 主题切换：一屏一次全刷实测约 130ms，做不出中间帧，直接落到位。
static uint8_t s_theme_idx;
static int64_t s_theme_key_us;

extern const uint8_t themes_bin_start[] asm("_binary_themes_bin_start");
extern const uint8_t sprites_bin_start[] asm("_binary_sprites_bin_start");

#define TH (THEMES[s_theme_idx])

static void theme_set_src(void);
static void theme_apply_colors(void);
static void theme_step(int dir);
static void play_sound(uint32_t id);
static void handle_key(uint8_t btn, uint8_t ev);

// 召唤场景
static lv_obj_t *s_pet_scr;
static lv_obj_t *s_pet_title;
static lv_obj_t *s_pet_batt;
static lv_obj_t *s_pet_root;
static lv_obj_t *s_glow;
static lv_obj_t *s_art;
static lv_obj_t *s_spark[SPARKS];
static lv_obj_t *s_pet_hint;      // 底部提示行（右基线 218,296，8px）
static lv_obj_t *s_pet_sub;       // 按键行左（20,306，8px）
static lv_obj_t *s_pet_key;       // 按键行右（右对齐 220,306，8px）
static lv_obj_t *s_pet_toast_box;
static lv_obj_t *s_pet_toast;
static lv_obj_t *s_pet_env;
static lv_obj_t *s_flash;

// 设置页
static lv_obj_t *s_set_scr;
static lv_obj_t *s_set_name[SET_ROWS];
static lv_obj_t *s_set_val[SET_ROWS];
static lv_obj_t *s_set_cursor[SET_ROWS];
static lv_obj_t *s_set_hint;
static lv_obj_t *s_set_toast_box;
static lv_obj_t *s_set_toast;
static uint8_t s_set_cursor_row;

// 门户页
static lv_obj_t *s_portal_scr;
static lv_obj_t *s_portal_ssid;
static lv_obj_t *s_portal_pass;
static lv_obj_t *s_portal_left;
static lv_obj_t *s_portal_hint;
static lv_obj_t *s_portal_toast_box;
static lv_obj_t *s_portal_toast;

static QueueHandle_t s_input_q;
static QueueHandle_t s_snd_q;

static volatile bool s_running_snd;    // 音频任务正在播，UI 线程只读
static volatile uint32_t s_cur_snd;    // 正在播的 id，用于同类去重
static volatile uint32_t s_snd_pending; // 已排队 id 位图
static int64_t s_last_cat_snd_ms;      // 上一条猫叫时间戳，用于最小间隔

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
// 省电：无操作到息屏阈值的一半 → 背光最低 + 暂停待机动画；再继续走原有息屏。
static bool s_low_power;
static bool s_need_save;
static bool s_set_dirty;

// 领养纪念：首次领养时间落 pet 存档；未校时先记本次开机锚点，校时后补齐。
static int64_t s_adopt_boot_us = -1;
static bool s_adopt_pending;
static bool s_zoom;              // 长按 UP 的纯观看态
static lv_obj_t *s_plate_box;    // 铭牌底板
static lv_obj_t *s_plate_l1;     // 领养日期 / 第 N 天
static lv_obj_t *s_plate_l2;     // 已陪伴天数

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

// ---------------------------------------------------------------------------
// 宠物精灵：预渲染八帧 + 猫窝 + 眼镜。运行时仅 3 个 image 对象，
// 切状态只换 image 源与坐标，不重建图层、不新增对象，静态占 .rodata。
// sprites.bin 为 RGB565A8（彩色面 + 透明面），保证猫与猫窝叠层边缘干净。
// ---------------------------------------------------------------------------
#define ART_W 180
#define ART_H 196
#define ART_X 30
#define ART_Y 14

extern const uint8_t sprites_bin_start[] asm("_binary_sprites_bin_start");

typedef enum {
    CAT_PLAY = 0,
    CAT_COZY,
    CAT_SLEEP,
} cat_state_t;

// 短时表情：到点自动回静息帧，不排队、不叠加。
typedef enum {
    FX_NONE = 0,
    FX_HAPPY,
    FX_CURIOUS,
} cat_fx_t;

static cat_state_t s_cat_state = CAT_COZY;
static cat_fx_t s_fx = FX_NONE;
static int32_t s_fx_ms;
static bool s_peek;      // 夜里被看一眼，睁眼但不叫醒
static int32_t s_wake_ms;
static uint8_t s_frame = 0xFF;
static int16_t s_slide;  // 跳跃左右位移
static int16_t s_lift;   // 跳跃/呼吸纵向位移

static lv_obj_t *s_bed;
static lv_obj_t *s_cat;
static lv_obj_t *s_acc_face;   // 眼镜 / 酷墨镜（互斥，单值枚举）
static lv_obj_t *s_acc_body;   // 上班族西装 / 酷外套
static lv_obj_t *s_yarn;       // 毛线球（只跟玩毛线球姿态一起出现）
static lv_image_dsc_t s_dsc[SPR_COUNT];
static bool s_spr_ready;
static volatile uint32_t s_render_cnt;

// 待机随机小动作：0=无 1=打滚 2=玩毛线球。只影响画面，不发声、不走互动冷却。
static uint8_t s_pose;
static int32_t s_pose_ms;

// ---- 测试仪表：只有 FAP_TEST 会打开，正常使用零开销 ----
static bool s_lat_on;                       // 按键时延日志开关
static volatile uint32_t s_lat_key_ms;      // 按键入队时刻
static volatile bool s_lat_pending;         // 等待下一次渲染完成
static char s_bench_tag[16];
static bool s_bench_on;
static uint32_t s_bench_t0, s_bench_f0, s_bench_tick0, s_bench_sec;
static uint32_t s_tick_cnt;                 // ui_tick 次数：测主循环实际频率
static int s_test_keys, s_test_theme, s_test_audio, s_test_anim;
static uint32_t s_test_seq, s_test_anim_next;
static uint32_t s_test_next_ms;
static uint32_t s_soak_end_ms, s_soak_heap_min;

// 真实刷新计数：LVGL 每完成一次屏幕渲染 +1，用于现场测 fps。
static void on_render_ready(lv_event_t *e) {
    (void)e;
    s_render_cnt++;
    // 按下→画面真正刷完的毫秒数，比"函数返回"更接近肉眼感受。
    if (s_lat_pending) {
        s_lat_pending = false;
        ESP_LOGI(TAG, "LAT ms=%u", (unsigned)(lv_tick_get() - s_lat_key_ms));
    }
}

// 帧选择：特写 > 睡觉 > 随机小动作 > 短时表情 > 场景静息姿态。
static uint8_t frame_for_state(void) {
    if (s_zoom) return SPR_CLOSE;
    if (s_cat_state == CAT_SLEEP) return s_peek ? SPR_CURIOUS : SPR_SLEEP;
    if (s_pose == 1) return SPR_POSE_ROLL;
    if (s_pose == 2) return SPR_POSE_YARN;
    if (s_fx == FX_HAPPY) return SPR_HAPPY;
    if (s_fx == FX_CURIOUS) return SPR_CURIOUS;
    return s_cat_state == CAT_PLAY ? SPR_POUNCE : SPR_IDLE;
}

// 把 sprites.bin 的裸像素包装成 lv_image_dsc_t，零拷贝（指针直接指向 rodata）。
static void spr_setup(void) {
    if (s_spr_ready) return;
    s_spr_ready = true;
    for (int i = 0; i < SPR_COUNT; i++) {
        s_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        s_dsc[i].header.cf = LV_COLOR_FORMAT_RGB565A8;
        s_dsc[i].header.flags = 0;
        s_dsc[i].header.w = SPR[i].w;
        s_dsc[i].header.h = SPR[i].h;
        s_dsc[i].header.stride = SPR[i].w * 2;
        s_dsc[i].data_size = (uint32_t)SPR[i].w * SPR[i].h * 3;
        s_dsc[i].data = sprites_bin_start + SPR[i].off;
    }
}

// 配件：单值枚举保证互斥；同一时刻最多一件。
// 眼镜（GLASS）已下线：旧档里 acc==1 一律当"未戴"处理，不崩、不改档。
// 面部只保留酷墨镜，按鼻梁锚点对齐；身体（西装/外套）只在 idle 帧显示。
static void acc_apply(void) {
    if (!s_acc_face || !s_acc_body) return;
    const spr_info_t *s = &SPR[s_frame];
    const uint8_t acc = s_profile.acc;

    // 面部：只有 COOL(3)；GLASS(1) 视作 OFF
    const bool face_want = (acc == 3) && s->kind == 0 && !s->face;
    if (!face_want) {
        lv_obj_add_flag(s_acc_face, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_image_set_src(s_acc_face, &s_dsc[SPR_ACC_COOL_FACE]);
        lv_obj_remove_flag(s_acc_face, LV_OBJ_FLAG_HIDDEN);
        const uint32_t zoom = (uint32_t)s->eye_d * 256 / ACC_REF_EYE_D;
        lv_image_set_scale(s_acc_face, zoom);
        lv_image_set_pivot(s_acc_face, ACC_COOL_BRIDGE_X, ACC_COOL_BRIDGE_Y);
        lv_image_set_rotation(s_acc_face, (int32_t)s->eye_ang - ACC_REF_EYE_ANG);
        // 镜桥 = 鼻梁：对象左上角 = 鼻梁 - 镜桥在 sprite 内的位置。
        lv_obj_set_pos(s_acc_face, s->ox + s_slide + s->nose_x - ACC_COOL_BRIDGE_X,
                       s->oy + s_lift + s->nose_y - ACC_COOL_BRIDGE_Y);
    }

    // 身体：IDLE / HAPPY / CURIOUS 共用同一张画布对齐的西装层，侧面与动态帧不穿。
    const bool body_want = (acc == 2 || acc == 3) &&
                           (s_frame == SPR_IDLE || s_frame == SPR_HAPPY ||
                            s_frame == SPR_CURIOUS);
    if (!body_want) {
        lv_obj_add_flag(s_acc_body, LV_OBJ_FLAG_HIDDEN);
    } else {
        const uint8_t bspr = (acc == 3) ? SPR_ACC_COOL_BODY : SPR_ACC_OFFICE;
        lv_image_set_src(s_acc_body, &s_dsc[bspr]);
        lv_obj_remove_flag(s_acc_body, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_scale(s_acc_body, 256);
        lv_image_set_rotation(s_acc_body, 0);
        // 用配件自己的画布锚点，不能借当前猫帧的裁剪原点。
        lv_obj_set_pos(s_acc_body, SPR[bspr].ox + s_slide, SPR[bspr].oy + s_lift);
    }

    // 毛线球只在玩毛线球姿态出现，和猫一起做同样的位移。
    if (s_yarn) {
        if (s_pose != 2) {
            lv_obj_add_flag(s_yarn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_yarn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_yarn, SPR[SPR_YARN_BALL].ox + s_slide,
                           SPR[SPR_YARN_BALL].oy + s_lift);
        }
    }
}

// 首页主视觉猫的配件层：与宠物页同一套锚点规则（全部用配件自己的画布坐标）。
static void home_acc_apply(void) {
    if (!s_home_acc_body || !s_home_acc_face) return;
    const uint8_t acc = s_profile.acc;
    // 主视觉固定为主人照片，STAGE 里没有猫，配件层不再叠加（宠物页仍有自己的配件）。
    const bool cat_hero = false;

    const bool face_want = cat_hero && (acc == 3);
    if (!face_want) {
        lv_obj_add_flag(s_home_acc_face, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_image_set_src(s_home_acc_face, &s_dsc[SPR_ACC_COOL_FACE]);
        lv_obj_remove_flag(s_home_acc_face, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_scale(s_home_acc_face, 256);
        lv_image_set_pivot(s_home_acc_face, ACC_COOL_BRIDGE_X, ACC_COOL_BRIDGE_Y);
        lv_image_set_rotation(s_home_acc_face,
                              (int32_t)SPR[SPR_IDLE].eye_ang - ACC_REF_EYE_ANG);
        lv_obj_set_pos(s_home_acc_face,
                       HOME_STAGE_X + SPR[SPR_IDLE].ox + SPR[SPR_IDLE].nose_x -
                           ACC_COOL_BRIDGE_X,
                       HOME_STAGE_Y + SPR[SPR_IDLE].oy + SPR[SPR_IDLE].nose_y -
                           ACC_COOL_BRIDGE_Y);
    }

    const bool body_want = cat_hero && (acc == 2 || acc == 3);
    if (!body_want) {
        lv_obj_add_flag(s_home_acc_body, LV_OBJ_FLAG_HIDDEN);
    } else {
        const uint8_t bspr = (acc == 3) ? SPR_ACC_COOL_BODY : SPR_ACC_OFFICE;
        lv_image_set_src(s_home_acc_body, &s_dsc[bspr]);
        lv_obj_remove_flag(s_home_acc_body, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_scale(s_home_acc_body, 256);
        lv_image_set_rotation(s_home_acc_body, 0);
        lv_obj_set_pos(s_home_acc_body, HOME_STAGE_X + SPR[bspr].ox,
                       HOME_STAGE_Y + SPR[bspr].oy);
    }
}

// 只改 image 源与坐标；同一帧重复调用不触发重新解码。
static void apply_frame(uint8_t idx) {
    if (!s_cat) return;
    if (idx != s_frame) {
        s_frame = idx;
        lv_image_set_src(s_cat, &s_dsc[idx]);
    }
    lv_obj_set_pos(s_cat, SPR[s_frame].ox + s_slide, SPR[s_frame].oy + s_lift);
    acc_apply();
}

static void draw_pet(void) {
    if (!s_art) return;
    lv_obj_set_style_opa(s_art, LV_OPA_COVER, 0);
    s_slide = 0;
    s_lift = 0;
    apply_frame(frame_for_state());
}

// ---------------------------------------------------------------------------
// 坐久了自己蹦两下：蓄力（压扁帧）→ 腾空（拉长帧 + 抛物线）→ 落地（压扁帧回弹）。
// 帧驱动 + 位移，单次 ≤600ms；结束一定回到静息帧，不留中间态。
// ---------------------------------------------------------------------------
typedef enum {
    HOP_OFF = 0,
    HOP_CROUCH,
    HOP_AIR,
    HOP_LAND,
    HOP_SETTLE,
} hop_t;

static hop_t s_hop = HOP_OFF;
static int32_t s_hop_ms;
static uint8_t s_hop_jump;
static int32_t s_idle_ms;

#define HOP_JUMPS 3
static const int8_t HOP_H[HOP_JUMPS] = {14, 18, 12};
static const int8_t HOP_DX[HOP_JUMPS] = {-10, 8, -4};

static void hop_end(void) {
    s_hop = HOP_OFF;
    s_slide = 0;
    s_lift = 0;
    apply_frame(frame_for_state());
}

static void hop_begin(void) {
    if (s_cat_state == CAT_SLEEP || s_hop != HOP_OFF) return;
    s_hop = HOP_CROUCH;
    s_hop_ms = 0;
    s_hop_jump = 0;
    s_idle_ms = 0;
    s_slide = 0;
    s_lift = 0;
}

static void hop_tick(int32_t dt) {
    s_hop_ms += dt;
    switch (s_hop) {
    case HOP_CROUCH: {
        const int32_t q = s_hop_ms * 100 / 150;
        apply_frame(SPR_LAND);
        s_lift = (int16_t)(q < 100 ? 2 * q / 100 : 2);
        if (s_hop_ms >= 150) {
            s_hop = HOP_AIR;
            s_hop_ms = 0;
        }
        break;
    }
    case HOP_AIR: {
        const int32_t p = s_hop_ms * 100 / 280;
        const int32_t q = p < 100 ? p : 100;
        const int32_t arc = 4 * q * (100 - q) / 100;  // 抛物线：0 → 峰 → 0
        s_lift = (int16_t)(-HOP_H[s_hop_jump] * arc / 100);
        s_slide = (int16_t)(HOP_DX[s_hop_jump] * q / 100);
        apply_frame(SPR_AIR);
        if (p >= 100) {
            s_hop = HOP_LAND;
            s_hop_ms = 0;
        }
        break;
    }
    case HOP_LAND: {
        s_lift = 0;
        s_slide = HOP_DX[s_hop_jump];
        apply_frame(SPR_LAND);
        if (s_hop_ms >= 160) {
            s_hop_jump++;
            s_hop_ms = 0;
            s_hop = (s_hop_jump >= HOP_JUMPS) ? HOP_SETTLE : HOP_CROUCH;
        }
        break;
    }
    default: {
        s_lift = 0;
        apply_frame(frame_for_state());
        if (s_hop_ms >= 200) hop_end();
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// 对话气泡：猫头顶的圆角气泡 + 小尖角，跟随头部锚点。
// 淡入 120ms / 停留 1.8s / 淡出 180ms；同一时间只有一条，新的一条直接替换。
// ---------------------------------------------------------------------------
#define BUB_H 26
#define BUB_FADE_IN 120
#define BUB_FADE_OUT 180
#define BUB_HOLD 1800
#define BUB_TOP_MIN 30

static lv_obj_t *s_bub;
static lv_obj_t *s_bub_body;
static lv_obj_t *s_bub_tail;
static lv_obj_t *s_bub_lbl;
static int32_t s_bub_ms;
static bool s_bub_on;

static void bubble_build(lv_obj_t *parent) {
    s_bub = mk_clear(parent, 0, 40, 120, BUB_H);
    lv_obj_add_flag(s_bub, LV_OBJ_FLAG_HIDDEN);
    // 尖角先建，上半被气泡主体盖住，只露出下沿两条边。
    s_bub_tail = mk_box(s_bub, 55, BUB_H - 7, 10, 10, U_PAPER, 1);
    lv_obj_set_style_border_width(s_bub_tail, 1, 0);
    lv_obj_set_style_border_color(s_bub_tail, lv_color_hex(U_INK), 0);
    lv_obj_set_style_transform_rotation(s_bub_tail, 450, 0);
    s_bub_body = mk_box(s_bub, 0, 0, 120, BUB_H, U_PAPER, 11);
    lv_obj_set_style_border_width(s_bub_body, 1, 0);
    lv_obj_set_style_border_color(s_bub_body, lv_color_hex(U_INK), 0);
    // 浅底深字：气泡是纸上墨色，不能用暗色场景的 P_INK（浅字）。
    s_bub_lbl = mk_text(s_bub_body, "", &font_cjk_16, U_INK);
    lv_obj_center(s_bub_lbl);
}

// 右上双圆禁入区（美术坐标 116..194 / y 40..76，外扩 2px）。
#define RING_X0 114
#define RING_X1 196
#define RING_Y0 38
#define RING_Y1 78

static bool rect_hits_rings(int x, int y, int w, int h) {
    return !(x + w <= RING_X0 || x >= RING_X1 || y + h <= RING_Y0 || y >= RING_Y1);
}

// 优先放猫头顶；头顶被双圆占了就退到猫左侧、再退到下方，绝不压脸也不压合影位。
static void bubble_follow(void) {
    if (!s_bub || !s_bub_on) return;
    const spr_info_t *s = &SPR[s_frame];
    const int bw = lv_obj_get_width(s_bub);
    const int ax = ART_X + s->ox + s_slide + s->eye_x;
    // eye_y 是眼睛线，往上 40px 视为头顶，气泡吊在头顶上方。
    const int atop = ART_Y + s->oy + s_lift + s->eye_y - 40;

    static const int8_t CAND_X[4] = {0, -1, 0, -1};   // 0=居中对齐头部  -1=贴左边
    static const int8_t CAND_Y[4] = {0, 1, 2, 3};
    int bx = 4, by = 84;
    for (int i = 0; i < 4; i++) {
        int tx = (CAND_X[i] == 0) ? ax - bw / 2 : 4;
        int ty;
        switch (CAND_Y[i]) {
            case 0: ty = atop - 8 - BUB_H; break;      // 头顶上方
            case 1: ty = 82; break;                    // 左侧、双圆下沿以下
            case 2: ty = atop + 34; break;             // 头下方
            default: ty = 84; break;                   // 兜底
        }
        if (tx > SCREEN_W - bw - 2) tx = SCREEN_W - bw - 2;
        if (tx < 2) tx = 2;
        if (ty < BUB_TOP_MIN) ty = BUB_TOP_MIN;
        if (ty > 210 - BUB_H) ty = 210 - BUB_H;
        if (rect_hits_rings(tx, ty, bw, BUB_H)) continue;
        bx = tx;
        by = ty;
        break;
    }
    // 位置没变就不动对象：静态画面不产生多余 invalidate。
    static int last_x = -1, last_y = -1;
    if (bx != last_x || by != last_y) {
        last_x = bx;
        last_y = by;
        lv_obj_set_pos(s_bub, bx, by);
    }
    // 尖角指向猫头：气泡在猫右侧时落左端，在猫左侧时落右端。
    int tx = (ax > bx + bw) ? bw - 16 : ax - bx - 5;
    if (tx < 6) tx = 6;
    if (tx > bw - 16) tx = bw - 16;
    static int last_tx = -1;
    if (tx != last_tx) {
        last_tx = tx;
        lv_obj_set_x(s_bub_tail, tx);
    }
}

static void bubble_hide(void) {
    s_bub_on = false;
    if (s_bub) lv_obj_add_flag(s_bub, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------------------
// 本地文案池：全设备只此一处。按时间段 / 天气 / 互动次数分流。
// 下一版接在线更新时，只替换这张表的数据来源即可，调用方不用改。
// ---------------------------------------------------------------------------
enum {
    LY_GREET = 0,   // 召唤/回到场景
    LY_NIGHT,       // 夜间通用
    LY_RAIN,        // 雨雪雾天
    LY_HOT,         // 晴朗/高温
    LY_YOUNG,       // 成长值未满
    LY_OLD,         // 成长值已满
    LY_PET,         // 被摸
    LY_FEED,        // 被喂
    LY_EVOLVE,      // 跨阶段
    LY_IDLE,        // 待机随机
};

typedef struct {
    uint8_t cat;
    const char *txt;
} pet_line_t;

static const pet_line_t PET_LINES[] = {
    {LY_GREET, "我在这儿"},   {LY_GREET, "你来啦"},
    {LY_GREET, "等你好久了"}, {LY_GREET, "喵～想你了"},
    {LY_GREET, "欢迎回来"},   {LY_GREET, "今天也一起吧"},
    {LY_GREET, "我一直在等你"}, {LY_GREET, "醒来就看见你"},
    {LY_GREET, "这一面只给你看"}, {LY_GREET, "好久不见"},
    {LY_GREET, "我很好，你呢"}, {LY_GREET, "摸摸头再走"},
    {LY_GREET, "先陪我坐一会儿"}, {LY_GREET, "见到你就开心"},

    {LY_NIGHT, "夜深了"},     {LY_NIGHT, "该睡啦"},
    {LY_NIGHT, "一起睡吧"},   {LY_NIGHT, "小声一点哦"},
    {LY_NIGHT, "月亮出来了"}, {LY_NIGHT, "困得睁不开眼"},
    {LY_NIGHT, "晚安，明天见"}, {LY_NIGHT, "夜里我守着"},
    {LY_NIGHT, "做个好梦"},   {LY_NIGHT, "灯关了也不怕"},

    {LY_RAIN, "外面在下雨"},  {LY_RAIN, "听，雨声"},
    {LY_RAIN, "雨天不出门"},  {LY_RAIN, "窝里最舒服"},
    {LY_RAIN, "淋湿了会感冒"}, {LY_RAIN, "雨停再玩吧"},
    {LY_RAIN, "今天适合发呆"}, {LY_RAIN, "我陪你听雨"},

    {LY_HOT, "今天有点热"},   {LY_HOT, "晒得暖暖的"},
    {LY_HOT, "想喝点水"},     {LY_HOT, "太阳好大"},
    {LY_HOT, "找个阴凉处"},   {LY_HOT, "夏天真长"},
    {LY_HOT, "热得不想动"},   {LY_HOT, "风扇借我一下"},

    {LY_YOUNG, "我还小小的"}, {LY_YOUNG, "抱得动我吗"},
    {LY_YOUNG, "世界好大"},   {LY_YOUNG, "什么都想闻一闻"},
    {LY_YOUNG, "第一次见这个"}, {LY_YOUNG, "我会长大的"},
    {LY_YOUNG, "教教我吧"},   {LY_YOUNG, "今天学会新东西"},

    {LY_OLD, "我长大了"},     {LY_OLD, "你放心，我在"},
    {LY_OLD, "陪你的第好多天"}, {LY_OLD, "现在换我照顾你"},
    {LY_OLD, "我认得你的脚步"}, {LY_OLD, "习惯了你在身边"},
    {LY_OLD, "老搭档了"},     {LY_OLD, "还是最喜欢你"},

    {LY_PET, "陪我玩一下"},   {LY_PET, "再摸一下"},
    {LY_PET, "这里最舒服"},   {LY_PET, "好舒服呀"},
    {LY_PET, "手别停"},       {LY_PET, "蹭蹭你"},
    {LY_PET, "我开心"},       {LY_PET, "轻一点哦"},
    {LY_PET, "要抱抱"},       {LY_PET, "喜欢你身上的味道"},
    {LY_PET, "今天也很乖"},   {LY_PET, "摸头最好了"},
    {LY_PET, "再靠近一点"},   {LY_PET, "别走开"},

    {LY_FEED, "吃饱啦"},      {LY_FEED, "还有吗"},
    {LY_FEED, "这个好吃"},    {LY_FEED, "谢谢你"},
    {LY_FEED, "吃得很满足"},  {LY_FEED, "小口小口吃"},
    {LY_FEED, "留一点晚上"},  {LY_FEED, "碗空了"},
    {LY_FEED, "闻起来真香"},  {LY_FEED, "再来一口"},

    {LY_EVOLVE, "更喜欢你啦"}, {LY_EVOLVE, "我好像长大了一点"},
    {LY_EVOLVE, "谢谢你养我"}, {LY_EVOLVE, "感觉不一样了"},
    {LY_EVOLVE, "我们更熟了"}, {LY_EVOLVE, "一起继续吧"},

    {LY_IDLE, "发呆中"},      {LY_IDLE, "看看窗外"},
    {LY_IDLE, "今天也很好"},  {LY_IDLE, "我在守着家"},
    {LY_IDLE, "有点无聊"},    {LY_IDLE, "你在忙什么"},
    {LY_IDLE, "打个哈欠"},    {LY_IDLE, "尾巴痒痒的"},
    {LY_IDLE, "想晒晒太阳"},  {LY_IDLE, "听到脚步声了"},
    {LY_IDLE, "安静真好"},    {LY_IDLE, "数一数云"},
    {LY_IDLE, "好像要下雨"},  {LY_IDLE, "再睡一小会儿"},
};

static const char *line_pick(uint8_t cat) {
    uint16_t idx[32];
    int n = 0;
    const int total = (int)(sizeof(PET_LINES) / sizeof(PET_LINES[0]));
    for (int i = 0; i < total && n < 32; i++) {
        if (PET_LINES[i].cat == cat) idx[n++] = (uint16_t)i;
    }
    if (n == 0) return "";
    return PET_LINES[idx[esp_random() % (uint32_t)n]].txt;
}

// 分流：待机时按夜间 / 天气 / 成长阶段插队，其余按动作类别。
static const char *line_for(uint8_t cat) {
    if (cat == LY_IDLE) {
        const uint32_t r = esp_random() % 100;
        if (r < 22) return line_pick(LY_NIGHT);
        if (r < 34) {
            const wx_kind_t wx = weather_kind();
            return line_pick(wx == WX_RAIN || wx == WX_SNOW || wx == WX_THUNDER
                                 ? LY_RAIN
                                 : LY_HOT);
        }
        if (r < 60) {
            return line_pick(s_pet.growth >= PET_GROWTH_TOP_STAGE ? LY_OLD : LY_YOUNG);
        }
    }
    return line_pick(cat);
}

static void bubble_say(const char *txt) {
    if (!s_bub || !txt || !txt[0]) return;
    lv_label_set_text(s_bub_lbl, txt);
    lv_point_t ts;
    lv_text_get_size(&ts, txt, &font_cjk_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int bw = ts.x + 18;
    if (bw < 56) bw = 56;
    if (bw > SCREEN_W - 4) bw = SCREEN_W - 4;
    lv_obj_set_width(s_bub, bw);
    lv_obj_set_width(s_bub_body, bw);
    lv_obj_set_pos(s_bub_tail, bw / 2 - 5, BUB_H - 7);
    lv_obj_set_style_opa(s_bub, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_bub, LV_OBJ_FLAG_HIDDEN);
    s_bub_ms = 0;
    s_bub_on = true;
    bubble_follow();
}

static void bubble_tick(int32_t dt) {
    if (!s_bub_on) return;
    s_bub_ms += dt;
    const int32_t out_at = BUB_FADE_IN + BUB_HOLD;
    int32_t opa;
    if (s_bub_ms < BUB_FADE_IN) {
        opa = 255 * s_bub_ms / BUB_FADE_IN;
    } else if (s_bub_ms < out_at) {
        opa = 255;
    } else if (s_bub_ms < out_at + BUB_FADE_OUT) {
        opa = 255 * (out_at + BUB_FADE_OUT - s_bub_ms) / BUB_FADE_OUT;
    } else {
        bubble_hide();
        return;
    }
    lv_obj_set_style_opa(s_bub, (lv_opa_t)opa, 0);
    bubble_follow();
}
// ---------------------------------------------------------------------------
// 状态与文案
// ---------------------------------------------------------------------------
static int64_t clock_now(void) {
    if (!s_time_known) return -1;
    return s_epoch_base + (esp_timer_get_time() - s_epoch_anchor_us) / 1000000;
}

// 卡面顶部状态行：本地时间（UTC+8）。未校时显示占位，不猜时间。
static void refresh_status(void) {
    if (!s_clock) return;
    const int64_t now = clock_now();
    if (now < 0) {
        lv_label_set_text(s_clock, "--:--");
        return;
    }
    const int64_t local = now + 8 * 3600;
    char b[8];
    snprintf(b, sizeof(b), "%02d:%02d", (int)((local / 3600) % 24),
             (int)((local / 60) % 60));
    lv_label_set_text(s_clock, b);
}

// 场景底部环境标签：本地时间（UTC+8）+ 天气短名；无时间/无天气显示占位。
static void refresh_env(void) {
    if (!s_pet_env) return;
    char b[28];
    const int64_t now = clock_now();
    if (now < 0) {
        snprintf(b, sizeof(b), "未校时");
    } else {
        const int64_t local = now + 8 * 3600;
        snprintf(b, sizeof(b), "%02d:%02d %s", (int)((local / 3600) % 24),
                 (int)((local / 60) % 60), weather_name(weather_kind()));
    }
    lv_label_set_text(s_pet_env, b);
}

// 文字基线 → 标签顶端。LVGL 首行基线 = top + (line_height - base_line)。
static int baseline_top(const lv_font_t *f, int baseline_y) {
    return baseline_y - (int)(lv_font_get_line_height(f) - f->base_line);
}

// 场景页底部署名：男/女两版合影按 profile.sex 选，1:1 原始像素，不缩放。
static const lv_image_dsc_t *duo_src(void) {
    spr_setup();
    return &s_dsc[s_profile.sex ? SPR_SIGN_F : SPR_SIGN_M];
}

// 落款名逐字查字模：LVGL 缺字不画方块、只表现为短一截，必须留日志不能静默。
// 走 LVGL 自己的 cmap（与渲染同一字体），比自建 unicode_list 表更可靠。
// 缺字只记日志，照常渲染——不要因为缺字就不显示名字。
static void petname_check_glyphs(const char *s) {
    char list[96] = {0};
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        uint32_t cp;
        int len;
        if (*p < 0x80) { cp = *p; len = 1; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; len = 2; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; len = 3; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; len = 4; }
        else { p++; continue; }
        for (int i = 1; i < len; i++) {
            if ((p[i] & 0xC0) != 0x80) { len = 0; break; }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if (len == 0) { p++; continue; }
        lv_font_glyph_dsc_t dsc;
        if (!lv_font_get_glyph_dsc(&font_cjk_8, &dsc, cp, 0)) {
            size_t left = sizeof(list) - strlen(list);
            if (left > 12) {
                snprintf(list + strlen(list), left, " U+%04X (%.*s)", (unsigned)cp, len,
                         (const char *)p);
            }
        }
        p += len;
    }
    if (list[0]) ESP_LOGW(TAG, "petname 缺字:%s", list);
}

// 统一刷新入口：换性别、换猫名、主题切换都走这里，不重建容器。
static void duo_apply(void) {
    if (!s_duo_box) return;
    const lv_image_dsc_t *src = duo_src();
    if (!src) return;
    lv_image_set_src(s_duo_img, src);
    const char *name = s_profile.petname[0] ? s_profile.petname : PROFILE_PETNAME_DEFAULT;
    petname_check_glyphs(name);
    char line[48];
    snprintf(line, sizeof(line), "你 和 %s", name);
    lv_label_set_text(s_duo_text, line);
    lv_obj_invalidate(s_duo_box);
}

static void duo_show(bool on) {
    if (!s_duo_box) return;
    if (on) {
        lv_obj_remove_flag(s_duo_box, LV_OBJ_FLAG_HIDDEN);
        if (s_duo_text) lv_obj_remove_flag(s_duo_text, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_duo_box, LV_OBJ_FLAG_HIDDEN);
        // 名字不是合影的子对象（要按绝对基线锚），显隐得一起管。
        if (s_duo_text) lv_obj_add_flag(s_duo_text, LV_OBJ_FLAG_HIDDEN);
    }
}

// dsc 几何固定 88x88 RGB565A8。主视觉 176 = 88x2（scale 512，整数倍，最近邻无相位跳变）。
static void av_geom(void) {
    s_av_dsc.header.w = PROFILE_AVATAR_W;
    s_av_dsc.header.h = PROFILE_AVATAR_W;
    s_av_dsc.header.stride = PROFILE_AVATAR_W * 2;
    s_av_dsc.data_size = PROFILE_AVATAR_BYTES;
    s_av_dsc.data = s_av_buf;
    if (s_hero_img) lv_image_set_scale(s_hero_img, 512);
}

// 首页主视觉优先级：88x88 真彩上传图 > 内置 p88 认可头像。
// 两条链路都写进同一块 88x88 RGB565A8 缓冲（23,232 B），只给 STAGE 的 2 倍主视觉用。
// 旧 512 B / 2,192 B 存档在 profile_load_avatar 里已经当作“无头像”，这里自然落回内置头像。
static void avatar_build(void) {
    // 门户期间缓冲已让给 WiFi：此时上传头像只写 NVS，不抢 23 KB 回来重建。
    if (s_av_released) return;
    if (s_av_heap && s_av_store) {
        free(s_av_store);
        s_av_store = NULL;
        s_av_heap = false;
    }
    s_av_released = false;

    if (profile_avatar_custom()) {
        s_av_store = malloc(PROFILE_AVATAR_BYTES);
        if (s_av_store) {
            const size_t n = profile_load_avatar(s_av_store, PROFILE_AVATAR_BYTES);
            if (n == PROFILE_AVATAR_BYTES) {
                s_av_buf = s_av_store;
                s_av_heap = true;
            } else {
                free(s_av_store);
                s_av_store = NULL;
            }
        }
    }
    if (!s_av_heap) {
        // 无档案 / 申请失败：直接用 flash 里的内置形象（按 M/F 取槽），零 DRAM。
        s_av_buf = (uint8_t *)avatar_default_slot(s_profile.sex);
    }
    av_geom();
    // 场景合影位：横版图按性别切换。
    duo_apply();
    // 门户退出后从 flash 重建：把证件照的源挂回去（门户期间被摘掉了）。
    if (s_hero_img) {
        lv_image_set_src(s_hero_img, &s_av_dsc);
        lv_obj_invalidate(s_hero_img);
    }
}

// 门户期间首页主角整体让位：证件照先摘源（门户页自己画占位），再丢掉 LVGL 解码缓存
// 里那份头像，最后释放我们自己的 23 KB 堆缓冲。调用方必须已持有 LVGL 锁。
static void avatar_release_for_portal(void) {
    lv_mem_monitor_t m0;
    lv_mem_monitor(&m0);
    const uint32_t h0 = esp_get_free_heap_size();

    s_av_released = true;
    if (s_hero_img) lv_image_set_src(s_hero_img, NULL);
    lv_image_cache_drop(&s_av_dsc);
    if (s_av_heap && s_av_store) {
        free(s_av_store);
        s_av_store = NULL;
        s_av_heap = false;
    }

    lv_mem_monitor_t m1;
    lv_mem_monitor(&m1);
    ESP_LOGI(TAG, "门户让位: heap %u→%u (省 %d), pool_free %u→%u (省 %d), largest=%u",
             (unsigned)h0, (unsigned)esp_get_free_heap_size(),
             (int)esp_get_free_heap_size() - (int)h0,
             (unsigned)m0.free_size, (unsigned)m1.free_size,
             (int)m1.free_size - (int)m0.free_size,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

// 主视觉固定为主人照片：STAGE 只放 4 倍证件照，猫只留在右下角。
// 只切可见性，不重建整屏，也不碰宠物状态。
static void hero_apply(void) {
    if (s_home_cat) lv_obj_add_flag(s_home_cat, LV_OBJ_FLAG_HIDDEN);
    if (s_hero_box) lv_obj_remove_flag(s_hero_box, LV_OBJ_FLAG_HIDDEN);
    home_acc_apply();
}

// 头像像素重建 + 主视觉显隐：所有“头像变了”的路径都走这里。
static void avatar_apply(void) {
    avatar_build();
    hero_apply();
}

// 首页只刷新身份牌字段：时间 / 昵称 / 签名（省略号截断）。
static void refresh_home(void) {
    if (!s_home_scr) return;
    lv_label_set_text(s_nick, s_profile.nick[0] ? s_profile.nick : "UNNAMED");
    lv_label_set_text(s_sign, s_profile.sign[0] ? s_profile.sign : "Wear it. Flash it.");
    refresh_status();
}

static void refresh_pet_scene(void) {
    if (!s_pet_scr) return;
    // 不出现等级/阶段/成长：标题只放一句陪伴称呼。
    lv_label_set_text(s_pet_title, "LITTLE COMPANION");
    // 互动说明全部压到最底部 8px：提示行 + 左右按键行，不占主视觉。
    lv_label_set_text(s_pet_hint, TR("长按 UP 拉近 · 长按 OK 收回", "HOLD UP ZOOM · HOLD OK BACK"));
    lv_label_set_text(s_pet_sub, TR("OK 摸摸", "OK PET"));
    lv_label_set_text(s_pet_key, TR("UP 开心 · DOWN 叫", "UP HAPPY · DOWN MEOW"));
    refresh_env();
}

// 已陪伴天数：有时间按墙钟算；没时间按本次开机计时给“第 N 天”，绝不显示未来日期。
static int adopt_days(void) {
    if (s_pet.adopt_utc > 0 && s_time_known) {
        int64_t now = clock_now();
        if (now < (int64_t)s_pet.adopt_utc) now = (int64_t)s_pet.adopt_utc;
        return (int)((now - (int64_t)s_pet.adopt_utc) / 86400) + 1;
    }
    if (s_adopt_boot_us < 0) return 1;
    const int64_t up = (esp_timer_get_time() - s_adopt_boot_us) / 86400000000LL;
    return (int)up + 1;
}

static void plate_refresh(void) {
    if (!s_plate_l1 || !s_plate_l2) return;
    const int days = adopt_days();
    char b1[40], b2[24];
    if (s_pet.adopt_utc > 0 && s_time_known) {
        time_t t = (time_t)((int64_t)s_pet.adopt_utc + 8 * 3600);
        struct tm tmv;
        gmtime_r(&t, &tmv);
        snprintf(b1, sizeof(b1), TR("领养于 %04d-%02d-%02d", "ADOPTED %04d-%02d-%02d"),
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
        snprintf(b2, sizeof(b2), TR("已陪伴 %d 天", "%d DAYS TOGETHER"), days);
        lv_label_set_text(s_plate_l1, b1);
        lv_label_set_text(s_plate_l2, b2);
        lv_obj_remove_flag(s_plate_l2, LV_OBJ_FLAG_HIDDEN);
    } else {
        // 未校时保护：只给“第 N 天”，不猜日期。
        snprintf(b1, sizeof(b1), TR("第 %d 天", "DAY %d"), days);
        lv_label_set_text(s_plate_l1, b1);
        lv_obj_add_flag(s_plate_l2, LV_OBJ_FLAG_HIDDEN);
    }
}

// 拉近：纯观看态。不动宠物状态、不触发互动、不打断音效。
static void zoom_enter(void) {
    if (s_zoom || !s_cat || !s_plate_box) return;
    s_zoom = true;
    // 观看态：把正在播的小动作收掉，只保留静态特写。
    s_pose = 0;
    s_pose_ms = 0;
    s_hop = HOP_OFF;
    // 预渲染近景帧，1:1 贴图，不做运行时缩放（呼吸位移仍由 apply_frame 带）。
    s_lift = 0;
    s_slide = 0;
    apply_frame(SPR_CLOSE);
    if (s_bed) lv_obj_add_flag(s_bed, LV_OBJ_FLAG_HIDDEN);
    if (s_yarn) lv_obj_add_flag(s_yarn, LV_OBJ_FLAG_HIDDEN);
    if (s_acc_body) lv_obj_add_flag(s_acc_body, LV_OBJ_FLAG_HIDDEN);
    if (s_acc_face) lv_obj_add_flag(s_acc_face, LV_OBJ_FLAG_HIDDEN);
    if (s_pet_hint) lv_obj_add_flag(s_pet_hint, LV_OBJ_FLAG_HIDDEN);
    if (s_pet_sub) lv_obj_add_flag(s_pet_sub, LV_OBJ_FLAG_HIDDEN);
    if (s_pet_key) lv_obj_add_flag(s_pet_key, LV_OBJ_FLAG_HIDDEN);
    if (s_pet_env) lv_obj_add_flag(s_pet_env, LV_OBJ_FLAG_HIDDEN);
    // 特写态只留猫脸与铭牌：右上合影位、气泡一律让开。
    if (s_duo_box) duo_show(false);
    bubble_hide();
    plate_refresh();
    lv_obj_remove_flag(s_plate_box, LV_OBJ_FLAG_HIDDEN);
}

static void zoom_exit(void) {
    if (!s_zoom) return;
    s_zoom = false;
    if (s_cat) {
        lv_image_set_pivot(s_cat, 0, 0);
        lv_image_set_scale(s_cat, 256);
    }
    if (s_plate_box) lv_obj_add_flag(s_plate_box, LV_OBJ_FLAG_HIDDEN);
    if (s_bed) lv_obj_remove_flag(s_bed, LV_OBJ_FLAG_HIDDEN);
    apply_frame(frame_for_state());
    if (s_pet_hint) lv_obj_remove_flag(s_pet_hint, LV_OBJ_FLAG_HIDDEN);
    if (s_pet_sub) lv_obj_remove_flag(s_pet_sub, LV_OBJ_FLAG_HIDDEN);
    if (s_pet_key) lv_obj_remove_flag(s_pet_key, LV_OBJ_FLAG_HIDDEN);
    if (s_pet_env) lv_obj_remove_flag(s_pet_env, LV_OBJ_FLAG_HIDDEN);
    if (s_duo_box) duo_show(true);
    refresh_pet_scene();
}

static void show_toast(view_t view, const char *text, uint32_t ms);
static void enter_view(view_t view);

// 开热点是重活：必须离开 LVGL/串口任务的调用栈（深调用 + 小栈会直接复位）。
// 独立任务 8KB 栈，UI 只负责立刻回一句 STARTING，结果再回来刷新界面。
static void portal_open_task(void *arg) {
    (void)arg;
    // 开热点前先让出 23 KB 头像堆缓冲（dsc 先指回 flash），否则 WiFi 初始化必 OOM。
    if (bsp_lvgl_lock(2000)) {
        avatar_release_for_portal();
        bsp_lvgl_unlock();
    }
    const bool ok = wifi_portal_open();
    ESP_LOGI(TAG, "门户任务栈余量=%uB", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    bsp_lvgl_lock(2000);
    if (ok) {
        lv_label_set_text(s_portal_ssid, wifi_portal_ssid());
        lv_label_set_text(s_portal_pass, wifi_portal_pass());
        enter_view(VIEW_PORTAL);
    } else {
        // 失败原因直接上屏（LOW MEM / AP FAIL / HTTP FAIL），不静默、不重启。
        char msg[48];
        snprintf(msg, sizeof(msg), "FAIL: %s", wifi_portal_last_error());
        if (s_set_val[SET_ROW_PORTAL]) lv_label_set_text(s_set_val[SET_ROW_PORTAL], wifi_portal_last_error());
        enter_view(VIEW_SET);
        show_toast(VIEW_SET, msg, 2500);
    }
    bsp_lvgl_unlock();
    vTaskDelete(NULL);
}

static void portal_open_async(void) {
    if (s_set_val[SET_ROW_PORTAL]) lv_label_set_text(s_set_val[SET_ROW_PORTAL], "STARTING");
    lv_refr_now(NULL);   // 先给即时反馈，避免按键像没反应
    // 8KB：WiFi 延迟初始化（esp_wifi_init）也在这个任务里跑，6KB 偏紧。
    if (xTaskCreate(portal_open_task, "portal", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "开热点失败: 任务创建失败（内存不足）");
        if (s_set_val[SET_ROW_PORTAL]) lv_label_set_text(s_set_val[SET_ROW_PORTAL], "LOW MEM");
        enter_view(VIEW_SET);
        show_toast(VIEW_SET, "FAIL: LOW MEM", 2500);
    }
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
    // 离开场景即退出拉近观看态，按键语义随之恢复。
    if (v != VIEW_PET && s_zoom) zoom_exit();
    s_view = v;
    if (v == VIEW_HOME) {
        // 门户期间头像堆缓冲让给 WiFi 了，回首页时按 NVS 重新申请、重读。
        if (s_av_released) {
            s_av_released = false;
            avatar_build();
        }
        refresh_home();
        lv_screen_load(s_home_scr);
        // 入场只有位移：卡面从 +6px 滑入，180ms 内完成。
        s_home_in_ms = 0;
        if (s_home_body) lv_obj_set_pos(s_home_body, 16, 42);
    } else if (v == VIEW_PET) {
        refresh_pet_scene();
        lv_screen_load(s_pet_scr);
        if (s_summoned) {
            bubble_say(line_for(LY_GREET));
        }
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
        lv_obj_set_pos(s_art, 0, 18 - (18 * t) / 900);
        lv_obj_set_style_opa(s_art, (lv_opa_t)(255 * t / 900), 0);

        lv_obj_remove_flag(s_glow, LV_OBJ_FLAG_HIDDEN);
        const int gw = 56 + (68 * t) / 900;
        const int gh = gw * 86 / 100;
        lv_obj_set_size(s_glow, gw, gh);
        lv_obj_set_pos(s_glow, (ART_W - gw) / 2, 96 - gh / 2);
        lv_obj_set_style_opa(s_glow, (lv_opa_t)(150 * (900 - t) / 900), 0);

        static const int8_t dx[SPARKS] = {0, 38, 38, 0, -38, -38};
        static const int8_t dy[SPARKS] = {-44, -22, 22, 44, 22, -22};
        for (int i = 0; i < SPARKS; i++) {
            lv_obj_remove_flag(s_spark[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_spark[i], 88 + dx[i] * t / 900 - 2,
                           92 + dy[i] * t / 900 - 2);
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

    // 互动回应改由"换一帧 + 头顶气泡"表达，不再浮字、不再整体抬升。

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
    const uint32_t now_ms = lv_tick_get();
    s_tick_cnt++;

    // FAP_TEST 注入：混合按键 / 主题连切 / 连发音频。全在 LVGL 锁内跑，不另建任务。
    if (s_test_keys > 0 && (int32_t)(now_ms - s_test_next_ms) >= 0) {
        s_test_next_ms = now_ms + 150;
        const int k = (int)(s_test_seq++ % 5);   // 0/1/2 短按 OK/UP/DOWN，3/4 长按 OK/UP
        s_test_keys--;
        const uint8_t b = (k == 0 || k == 3) ? BSP_BTN_OK
                        : (k == 1 || k == 4) ? BSP_BTN_UP : BSP_BTN_DOWN;
        const uint8_t ev = (k == 3 || k == 4) ? BSP_BTN_LONG : BSP_BTN_CLICK;
        if (s_lat_on) {                 // 注入路径同样计入"按下→刷完"时延
            s_lat_key_ms = now_ms;
            s_lat_pending = true;
        }
        handle_key(b, ev);
        if (s_test_keys == 0)
            ESP_LOGI(TAG, "TEST keys done heap=%uB", (unsigned)esp_get_free_heap_size());
    }
    if (s_test_theme > 0 && (int32_t)(now_ms - s_test_next_ms) >= 0) {
        s_test_next_ms = now_ms + 260;
        s_test_theme--;
        theme_step(1);
        if (s_test_theme == 0)
            ESP_LOGI(TAG, "TEST theme20 done heap=%uB", (unsigned)esp_get_free_heap_size());
    }
    if (s_test_audio > 0 && (int32_t)(now_ms - s_test_next_ms) >= 0) {
        s_test_next_ms = now_ms + 300;
        s_test_audio--;
        play_sound(SND_CURIOUS);
        if (s_test_audio == 0)
            ESP_LOGI(TAG, "TEST audio20 done heap=%uB", (unsigned)esp_get_free_heap_size());
    }
    if (s_bench_on && (int32_t)(now_ms - s_bench_t0) >= (int32_t)(s_bench_sec * 1000)) {
        const uint32_t el = now_ms - s_bench_t0;
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        ESP_LOGI(TAG,
                 "BENCH tag=%s sec=%u fps=%.1f tick=%u/s pool_max=%uB frag=%u%% heap=%uB",
                 s_bench_tag, (unsigned)(el / 1000),
                 (double)(s_render_cnt - s_bench_f0) * 1000.0 / (double)(el ? el : 1),
                 (unsigned)((s_tick_cnt - s_bench_tick0) * 1000u / (el ? el : 1)),
                 (unsigned)mon.max_used, (unsigned)mon.frag_pct,
                 (unsigned)esp_get_free_heap_size());
        s_bench_on = false;
    }
    // 连续重绘压力：每 tick 换一帧，配合 bench 测"动作中"的真实 fps。
    if (s_test_anim > 0 && (int32_t)(now_ms - s_test_anim_next) >= 0) {
        s_test_anim_next = now_ms;
        s_test_anim--;
        apply_frame((s_test_anim & 1) ? SPR_CAT_HAPPY : SPR_POSE_ROLL);
    }
    if (s_soak_end_ms) {
        const uint32_t h = esp_get_free_heap_size();
        if (h < s_soak_heap_min) s_soak_heap_min = h;
        if ((int32_t)(now_ms - s_soak_end_ms) >= 0) {
            ESP_LOGI(TAG, "TEST soak heap_min=%uB heap=%uB",
                     (unsigned)s_soak_heap_min, (unsigned)esp_get_free_heap_size());
            s_soak_end_ms = 0;
        }
    }

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
    } else if (s_view == VIEW_PET && s_summoned && !s_dimmed && !s_low_power) {
        // 省电态（s_low_power）整段跳过：呼吸起伏与待机小动作一起冻结，画面停在最后一帧。
        s_bob_ms += (int32_t)dt;
        // 互动后的开心表情过一会儿自然收回（只换帧，不重建对象）。
        if (s_fx_ms > 0) {
            s_fx_ms -= (int32_t)dt;
            if (s_fx_ms <= 0) {
                s_fx_ms = 0;
                s_fx = FX_NONE;
                apply_frame(frame_for_state());
            }
        }
        if (s_cat_state == CAT_SLEEP) {
            // 夜里：只有 1px 的缓慢呼吸，偶尔睁眼看一眼再睡回去。
            if (s_wake_ms > 0) {
                s_wake_ms -= (int32_t)dt;
                if (s_wake_ms <= 0) {
                    s_wake_ms = 0;
                    s_peek = false;
                    apply_frame(frame_for_state());
                }
            }
            s_lift = ((s_bob_ms / 900) % 2) ? -1 : 0;
            apply_frame(s_frame);
        } else if (s_hop != HOP_OFF) {
            hop_tick((int32_t)dt);
        } else if (s_pose) {
            // 待机小动作：打滚 / 玩毛线球。纯画面，不发声、不写互动时间，不碰 4 小时冷却。
            s_pose_ms -= (int32_t)dt;
            s_lift = 0;
            apply_frame(s_frame);
            if (s_pose_ms <= 0) {
                s_pose = 0;
                apply_frame(frame_for_state());
            }
        } else {
            // 白天：极缓的呼吸起伏 + 坐久了自己起来动一下。
            static const int8_t BOB_C[4] = {0, -1, -2, -1};
            s_lift = BOB_C[(s_bob_ms / 420) % 4];
            apply_frame(s_frame);
            s_idle_ms += (int32_t)dt;
            if (s_idle_ms >= 26000 && s_fx == FX_NONE && !s_zoom) {
                s_idle_ms = 0;
                // 待机时偶尔说一句，走统一文案池（按夜间/天气/成长值分流）。
                if (!s_bub_on && (esp_random() & 1)) bubble_say(line_for(LY_IDLE));
                const uint32_t r = esp_random() % 3;
                if (r == 0) {
                    hop_begin();
                } else {
                    s_pose = (r == 1) ? 1 : 2;   // 1=打滚 2=玩毛线球
                    s_pose_ms = 1100;
                    apply_frame(frame_for_state());
                }
            }
        }
        bubble_tick((int32_t)dt);
    }

    // 首页入场：仅位移，180ms 内到位后不再改坐标。
    if (s_view == VIEW_HOME && s_home_in_ms >= 0 && s_home_body) {
        s_home_in_ms += (int32_t)dt;
        int32_t t = s_home_in_ms;
        if (t >= 180) {
            t = 180;
            s_home_in_ms = -1;
        }
        // 从 (16,42) 线性归零；t=180 时必须是 (0,0)，否则整卡内容永久偏移。
        const int32_t k = 180 - t;
        lv_obj_set_pos(s_home_body, (16 * k) / 180, (42 * k) / 180);
    }

    // 天气/时段背景：只在宠物场景推进，颜色与元素透明度内部做平滑。
    if (s_view == VIEW_PET && s_pet_scr) {
        pet_bg_tick(dt, clock_now(), weather_kind());
    }

    // 每秒：冷却倒计时 + 门户倒计时/超时 + 环境标签
    s_cd_ms += (int32_t)dt;
    if (s_cd_ms >= 1000) {
        s_cd_ms = 0;
        if (s_view == VIEW_HOME || s_view == VIEW_PET) {
            refresh_status();
        }
        if (s_view == VIEW_PET) {
            refresh_env();
        }
        if (s_view == VIEW_PORTAL) {
            if (wifi_portal_expired()) {
                wifi_portal_close();
                enter_view(VIEW_HOME);
            } else {
                const long long left_us =
                    wifi_portal_idle_us() - ((long long)esp_timer_get_time() -
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
// 猫叫类声音（召唤/被摸/开饭）走采样，UI 提示音仍是短方波。
// 规则：夜间不叫；同一时间只播一条（新的直接丢）；相邻两条至少间隔 1.5s。
#define SND_MIN_GAP_MS 1500

static bool night_now(void);

static bool sound_is_cat(uint32_t id) {
    return id == SND_SUMMON || id == SND_PET || id == SND_FEED ||
           id == SND_CURIOUS || id == SND_KITTEN;
}

static void play_sound(uint32_t id) {
    if (!s_snd_q || s_profile.volume == 0) return;
    if (sound_is_cat(id)) {
        if (night_now()) return;
        const int64_t ms = esp_timer_get_time() / 1000;
        if (s_last_cat_snd_ms && ms - s_last_cat_snd_ms < SND_MIN_GAP_MS) return;
        s_last_cat_snd_ms = ms;
    }
    // 单声道队列：不打断当前播放；同类去重；最多排 2 条，满了直接丢。
    if (id == s_cur_snd) return;
    const uint32_t bit = 1u << id;
    if (s_snd_pending & bit) return;
    if (uxQueueSpacesAvailable(s_snd_q) == 0) return;
    if (xQueueSend(s_snd_q, &id, 0) == pdTRUE) s_snd_pending |= bit;
}

// 夜里 22:00-07:00 让它睡；没校时按白天算，不报错、不卡住。
static bool night_now(void) {
    const int64_t now = clock_now();
    if (now < 0) return false;
    const int h = (int)(((now + 8 * 3600) / 3600) % 24);
    return h >= 22 || h < 7;
}

// 切换场景（玩逗猫棒 / 猫窝撒娇 / 睡觉）。只换一次帧，不做高频重绘。
static void cat_set_state(cat_state_t st) {
    s_cat_state = st;
    s_wake_ms = 0;
    s_hop = HOP_OFF;
    s_pose = 0;
    s_idle_ms = 0;
    s_fx_ms = 0;
    s_fx = FX_NONE;
    s_peek = false;
    s_slide = 0;
    s_lift = 0;
    draw_pet();
    refresh_pet_scene();
}

// 互动只给回应，不给数值。逗猫棒可以随时玩，成长仍受冷却约束。
static void do_interact(pet_action_t act) {
    const int64_t now = clock_now();
    const int32_t left = pet_cooldown_left(&s_pet, now);

    // 被摸到就笑，过一会儿自然收回。
    s_fx = FX_HAPPY;
    s_fx_ms = 2400;
    s_idle_ms = 0;
    // 互动优先：打断待机自播（打滚/毛线球/蹦跳），否则笑脸会被 pose/hop 帧盖掉。
    s_pose = 0;
    s_pose_ms = 0;
    if (s_hop != HOP_OFF) hop_end();

    if (act == PET_ACT_PET) {
        if (s_cat_state == CAT_SLEEP) {
            s_fx = FX_CURIOUS;
            s_fx_ms = 2400;
            s_wake_ms = 2600;  // 轻轻睁眼看一眼，再睡回去
            s_peek = true;
            draw_pet();
            bubble_say(line_pick(LY_NIGHT));
            play_sound(SND_KITTEN);   // 夜间/轻柔场景：幼猫小声叫，不用大嗓门
            return;
        }
        bubble_say(line_for(LY_PET));
        play_sound(SND_PET);
    } else {
        bubble_say(line_for(LY_FEED));
        play_sound(SND_FEED);
    }
    draw_pet();

    if (left < 0) {
        // 没校时：先让它玩，成长等时间同步后再算。
        refresh_pet_scene();
        return;
    }
    if (left > 0) {
        bubble_say(line_pick(LY_NIGHT));
        refresh_pet_scene();
        return;
    }

    bool evolved = false;
    if (!pet_interact(&s_pet, act, now, &evolved)) return;
    s_need_save = true;
    if (evolved) {
        draw_pet();
        lv_obj_remove_flag(s_flash, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(s_flash, LV_OPA_TRANSP, 0);
        s_anim = ANIM_EVOLVE;
        s_anim_ms = 0;
        play_sound(SND_EVOLVE);
        bubble_say(line_for(LY_EVOLVE));
    }
    refresh_pet_scene();
}

// ---------------------------------------------------------------------------
// 设置页
// ---------------------------------------------------------------------------
static const char *SET_NAMES_ZH[SET_ROWS] = {
    "音量", "亮度", "息屏", "配件", "主题", "形象", "配网", "语言",
};
static const char *SET_NAMES_EN[SET_ROWS] = {
    "VOLUME", "BRIGHTNESS", "IDLE SCREEN", "ACCESSORY", "THEME", "AVATAR", "PHONE SETUP",
    "LANGUAGE",
};
#define SET_NAME(i) (s_profile.lang ? SET_NAMES_EN[i] : SET_NAMES_ZH[i])

// 主题名与发布页同一套措辞（4 套；第 4 套美术未出图，卡面暂为占位）。
static const char *THEME_NAMES_ZH[THEME_COUNT] = {"暖纸", "深色", "酷卡", "彩色水墨"};
static const char *THEME_NAMES_EN[THEME_COUNT] = {"Paper", "Dark", "Carbon", "Ink"};

// 面部配件：眼镜已下线，只剩 OFF / STUDY / COOL。
static const char *ACC_NAMES[] = {"OFF", "OFF", "STUDY", "COOL"};

static void settings_refresh(void) {
    for (int i = 0; i < SET_ROWS; i++) {
        const char *v = "";
        char b[16];
        switch (i) {
        case 0:
            if (s_profile.volume == 0) {
                snprintf(b, sizeof(b), "MUTE");
            } else {
                snprintf(b, sizeof(b), "%u%%", (unsigned)s_profile.volume);
            }
            v = b;
            break;
        case 1:
            snprintf(b, sizeof(b), "%u%%", (unsigned)s_profile.bright);
            v = b;
            break;
        case 2:
            v = s_profile.idle == 0 ? "OFF" : (s_profile.idle == 15 ? "15S" : "30S");
            break;
        case 3:
            v = ACC_NAMES[s_profile.acc <= 3 ? s_profile.acc : 0];
            break;
        case 4:
            v = (s_profile.lang ? THEME_NAMES_EN : THEME_NAMES_ZH)
                [s_theme_idx % THEME_COUNT];
            break;
        case 5:
            // 形象：内置两套（M/F）。有上传头像时上传图优先，这里只改回落槽。
            v = s_profile.sex ? "F" : "M";
            break;
        case SET_ROW_LANG:
            v = TR("中文", "English");
            break;
        default:
            v = "ENTER >";
            break;
        }
        lv_label_set_text(s_set_name[i], SET_NAME(i));
        // 中文行名用 16px 汉字字库；英文仍用 montserrat_14，不缩字号。
        lv_obj_set_style_text_font(s_set_name[i],
                                   s_profile.lang ? &lv_font_montserrat_14 : &font_cjk_16,
                                   0);
        lv_label_set_text(s_set_val[i], v);
        // 只有中文主题名/语言名需要汉字字库；其余数值沿用 montserrat_20。
        lv_obj_set_style_text_font(s_set_val[i],
                                   (!s_profile.lang && (i == 4 || i == SET_ROW_LANG))
                                       ? &font_cjk_16
                                       : &lv_font_montserrat_20,
                                   0);
        if (s_set_cursor[i]) {
            lv_obj_set_style_bg_color(s_set_cursor[i],
                                      lv_color_hex((uint8_t)i == s_set_cursor_row
                                                       ? T_ACCENT
                                                       : T_TRACK),
                                      0);
        }
    }
    if (s_set_hint) {
        lv_label_set_text(s_set_hint,
                          s_set_cursor_row == SET_ROW_PORTAL
                              ? TR("点按或长按 OK 打开配网", "TAP OR HOLD OK: OPEN PORTAL")
                              : TR("OK 切换     长按 OK 退出", "OK CHANGE      HOLD OK EXIT"));
        lv_obj_set_style_text_font(s_set_hint,
                                   s_profile.lang ? &lv_font_montserrat_14 : &font_cjk_16,
                                   0);
    }
}

// 语言切换后一次性刷新所有界面文案；不重建对象、不闪屏。
static void apply_lang(void) {
    settings_refresh();     // 设置页行名/值/提示
    refresh_pet_scene();    // 宠物页底部三行提示
    plate_refresh();        // 领养铭牌
    if (s_ttl) {
        lv_label_set_text(s_ttl, TR("AI 身份卡", "AI PASSPORT"));
        lv_obj_set_style_text_font(s_ttl,
                                   s_profile.lang ? &lv_font_montserrat_10 : &font_cjk_8, 0);
    }
    if (s_rev) {
        lv_label_set_text(s_rev, TR("编号 01", "NO. 01"));
        lv_obj_set_style_text_font(s_rev,
                                   s_profile.lang ? &lv_font_montserrat_10 : &font_cjk_8, 0);
    }
    if (s_portal_hint) {
        lv_label_set_text(s_portal_hint, TR("OK 退出", "OK EXIT"));
        lv_obj_set_style_text_font(s_portal_hint,
                                   s_profile.lang ? &lv_font_montserrat_14 : &font_cjk_16, 0);
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
        s_profile.volume = (uint8_t)((s_profile.volume + 10) % 110);
        bsp_audio_set_volume(s_profile.volume);
        /* 切主题/设置静音：不触发任何语音 */
        break;
    case 1: {
        uint8_t v = s_profile.bright;
        v = (v >= 100) ? BRIGHT_MIN : (uint8_t)(v + 10);
        s_profile.bright = v;
        bsp_display_backlight(v);
        /* 切主题/设置静音：不触发任何语音 */
        break;
    }
    case 2:
        s_profile.idle = (s_profile.idle == 0) ? 15 : (s_profile.idle == 15 ? 30 : 0);
        /* 切主题/设置静音：不触发任何语音 */
        break;
    case 3:
        // 配件：OFF → 上班族 → 酷 → OFF，立即生效并落盘（眼镜已下线，跳过 1）。
        s_profile.acc = (uint8_t)(s_profile.acc == 0 ? 2 : (s_profile.acc == 2 ? 3 : 0));
        profile_save(&s_profile);
        s_set_dirty = false;
        acc_apply();
        home_acc_apply();
        if (s_cat) lv_obj_invalidate(s_cat);
        /* 切主题/设置静音：不触发任何语音 */
        break;
    case 4:
        // 主题：与首页 UP/DOWN 共用同一函数，立即落盘；设置页只回显名字。
        theme_step(+1);
        settings_refresh();
        break;
    case 5:
        // 形象：M ↔ F，立即落盘并重建头像（上传图优先，无档案用这套内置）。
        s_profile.sex = s_profile.sex ? 0 : 1;
        profile_save(&s_profile);
        s_set_dirty = false;
        avatar_apply();
        /* 切主题/设置静音：不触发任何语音 */
        break;
    case SET_ROW_PORTAL:
        // 先给即时反馈再切 WiFi：open 会阻塞，不能让人以为按键没反应。
        portal_open_async();
        return;
    case SET_ROW_LANG:
        // 中文 ↔ English：立即落盘并整体刷新界面，不退出设置页。
        s_profile.lang = s_profile.lang ? 0 : 1;
        profile_save(&s_profile);
        s_set_dirty = false;
        apply_lang();
        return;
    default:
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
                // 白天进猫窝撒娇，夜里直接蜷着睡。
                s_cat_state = night_now() ? CAT_SLEEP : CAT_COZY;
                s_wake_ms = 0;
                // 召唤动画只改透明度/位移、不改帧：进来就摆笑脸，按一次键就能看到。
                s_fx = FX_HAPPY;
                s_fx_ms = 2400;
                draw_pet();
                lv_obj_set_pos(s_art, 0, 18);
                lv_obj_set_style_opa(s_art, LV_OPA_TRANSP, 0);
                enter_view(VIEW_PET);
                s_anim = ANIM_SUMMON;
                s_anim_ms = 0;
                play_sound(SND_SUMMON);
            } else if (btn == BSP_BTN_UP) {
                theme_step(-1);
            } else if (btn == BSP_BTN_DOWN) {
                theme_step(+1);
            }
            break;
        case VIEW_PET:
            if (s_zoom) break;   // 观看态：短按不改变任何宠物状态
            if (s_anim == ANIM_SUMMON || s_anim == ANIM_EVOLVE) break;
            if (btn == BSP_BTN_OK && s_summoned) {
                do_interact(PET_ACT_PET);   // 互动（摸）
            } else if (btn == BSP_BTN_UP && s_summoned) {
                cat_set_state(CAT_COZY);
                play_sound(SND_PET);        // 呼噜
                // 夜里猫叫被静音门整体拦掉，补一行无声音的气泡，避免按键像坏掉。
                if (night_now()) bubble_say(line_pick(LY_NIGHT));
            } else if (btn == BSP_BTN_DOWN && s_summoned) {
                cat_set_state(CAT_COZY);
                play_sound(night_now() ? SND_KITTEN : SND_CURIOUS);   // 叫一声
                if (night_now()) bubble_say(line_pick(LY_NIGHT));
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
                /* 切主题/设置静音：不触发任何语音 */
                enter_view(VIEW_SET);
            }
            break;
        }
        return;
    }

    if (ev != BSP_BTN_LONG) return;

    if (s_view == VIEW_PET) {
        // 长按 UP 拉近 / 长按 DOWN 缩回 / 长按 OK 收回卡面。
        if (btn == BSP_BTN_OK) {
            if (s_zoom) zoom_exit();
            s_summoned = false;
            s_anim = ANIM_NONE;
            enter_view(VIEW_HOME);
        } else if (btn == BSP_BTN_UP) {
            if (s_summoned && s_anim == ANIM_NONE) zoom_enter();
        } else if (btn == BSP_BTN_DOWN) {
            zoom_exit();
        }
        return;
    }

    if (s_view == VIEW_HOME && btn == BSP_BTN_OK) {
        s_set_cursor_row = 0;
        settings_refresh();
        enter_view(VIEW_SET);
        /* 切主题/设置静音：不触发任何语音 */
    } else if (s_view == VIEW_SET && btn == BSP_BTN_OK) {
        // 配网入口对长按同样生效：避免用户习惯性长按 OK 直接被退回首页。
        if (s_set_cursor_row == SET_ROW_PORTAL) {
            settings_change();
            return;
        }
        if (s_set_dirty) {
            profile_save(&s_profile);
            s_set_dirty = false;
        }
        /* 切主题/设置静音：不触发任何语音 */
        enter_view(VIEW_HOME);
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
    update_batt_label(s_batt, TH.screen_muted, H_DANGER);
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
// 音频合成：猫叫/呼噜/短鸣用预生成 PCM（16kHz 16bit 单声道，.rodata 嵌入）
// ---------------------------------------------------------------------------
extern const uint8_t snd_meow_bin_start[] asm("_binary_snd_meow_bin_start");
extern const uint8_t snd_meow_bin_end[] asm("_binary_snd_meow_bin_end");
extern const uint8_t snd_purr_bin_start[] asm("_binary_snd_purr_bin_start");
extern const uint8_t snd_purr_bin_end[] asm("_binary_snd_purr_bin_end");
extern const uint8_t snd_chirp_bin_start[] asm("_binary_snd_chirp_bin_start");
extern const uint8_t snd_chirp_bin_end[] asm("_binary_snd_chirp_bin_end");
// 真实猫叫（用户提供素材，16kHz/16bit/单声道裸 PCM，已裁静音并归一化到 -3dBFS）。
// 替换点：重新跑 tools/mk_sounds.py 生成同名 .bin 覆盖即可，代码不用动。
// 来源：01_cat_meow_cute / 04_cat_curious_meow / 03_kitten_tiny_meow / 02_cat_purr
extern const uint8_t snd_curious_bin_start[] asm("_binary_snd_curious_bin_start");
extern const uint8_t snd_curious_bin_end[] asm("_binary_snd_curious_bin_end");
extern const uint8_t snd_kitten_bin_start[] asm("_binary_snd_kitten_bin_start");
extern const uint8_t snd_kitten_bin_end[] asm("_binary_snd_kitten_bin_end");

// 分段送 I2S；只在音频任务里跑，不占 LVGL 线程。
static bool play_clip(const uint8_t *pcm, size_t bytes) {
    // 1024 帧（32ms）一块，连续阻塞写入；DMA 总缓冲 192ms，避免被渲染/WiFi 调度打断。
    int16_t buf[1024];
    for (size_t off = 0; off < bytes;) {
        size_t n = bytes - off;
        if (n > sizeof(buf)) n = sizeof(buf);
        memcpy(buf, pcm + off, n);
        if (bsp_audio_write(buf, n) != ESP_OK) return false;
        off += n;
    }
    return true;
}

static void synth_sound(uint32_t id) {
    // 猫叫类：直接放采样
    const uint8_t *clip = NULL;
    size_t clip_bytes = 0;
    switch (id) {
    case SND_SUMMON:
        clip = snd_meow_bin_start;
        clip_bytes = (size_t)(snd_meow_bin_end - snd_meow_bin_start);
        break;
    case SND_PET:
        clip = snd_purr_bin_start;
        clip_bytes = (size_t)(snd_purr_bin_end - snd_purr_bin_start);
        break;
    case SND_CURIOUS:
        clip = snd_curious_bin_start;
        clip_bytes = (size_t)(snd_curious_bin_end - snd_curious_bin_start);
        break;
    case SND_KITTEN:
        clip = snd_kitten_bin_start;
        clip_bytes = (size_t)(snd_kitten_bin_end - snd_kitten_bin_start);
        break;
    case SND_FEED:
        // 开饭：先短鸣，再呼噜一小段
        play_clip(snd_chirp_bin_start,
                  (size_t)(snd_chirp_bin_end - snd_chirp_bin_start));
        clip = snd_purr_bin_start;
        clip_bytes = (size_t)(snd_purr_bin_end - snd_purr_bin_start);
        break;
    default:
        break;
    }
    if (clip) {
        play_clip(clip, clip_bytes);
        return;
    }

    int dur_ms = 100;
    int f0 = 800;
    int f1 = 400;
    int amp = 3600;

    switch (id) {
    case SND_EVOLVE: dur_ms = 560; f0 = 520; f1 = 1320; amp = 5200; break;
    case SND_DENY: dur_ms = 180; f0 = 320; f1 = 210; amp = 3400; break;
    case SND_UI: dur_ms = 60; f0 = 880; f1 = 1040; amp = 2800; break;
    default: return;
    }

    const int total = 16000 * dur_ms / 1000;
    int16_t buf[1024];
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
        if (xQueueReceive(s_snd_q, &id, portMAX_DELAY) == pdTRUE) {
            s_snd_pending &= ~(1u << id);
            s_running_snd = true;
            s_cur_snd = id;
            synth_sound(id);
            s_cur_snd = 0;
            s_running_snd = false;
        }
    }
}

// ---------------------------------------------------------------------------
// 界面构建
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 首页主题：预渲染整卡底图 + 上层动态字段。切换只换底图源与文字色，不重排布局。
// ---------------------------------------------------------------------------
static void theme_set_src(void) {
    s_theme_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_theme_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_theme_dsc.header.flags = 0;
    s_theme_dsc.header.w = THEME_W;
    s_theme_dsc.header.h = THEME_H;
    s_theme_dsc.header.stride = THEME_W * 2;
    s_theme_dsc.data_size = THEME_BYTES;
    s_theme_dsc.data = themes_bin_start + TH.off;
    if (s_theme_bg) lv_image_set_src(s_theme_bg, &s_theme_dsc);
}

// 文字/配件锚点：三套卡面各自的 NAME/SECONDARY/MICRO/头像框位置。
// 切换主题只改坐标与颜色，不重建对象，不闪屏。
static void layout_apply(void) {
    const home_layout_t *L = &HOME_LAYOUT[s_theme_idx];
    if (s_nick) {
        lv_obj_set_pos(s_nick, L->name_x, L->name_y - 3);
        lv_obj_set_width(s_nick, L->name_w);
    }
    if (s_sign) {
        lv_obj_set_pos(s_sign, L->sub_x, L->sub_y);
        lv_obj_set_width(s_sign, L->sub_w);
    }
    // 酷卡文字牌只有 248..302，MICRO 行按美术标注"可省略"：直接隐藏，避免压签名与被头像框裁切。
    const bool cool = (s_theme_idx == 2);
    if (s_ttl) {
        if (cool) lv_obj_add_flag(s_ttl, LV_OBJ_FLAG_HIDDEN);
        else {
            lv_obj_remove_flag(s_ttl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_ttl, L->micro_l_x, L->micro_y - 1);
        }
    }
    if (s_rev) {
        if (cool) lv_obj_add_flag(s_rev, LV_OBJ_FLAG_HIDDEN);
        else {
            lv_obj_remove_flag(s_rev, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(s_rev, LV_ALIGN_TOP_RIGHT, 0 - (240 - L->micro_r_right), L->micro_y - 1);
        }
    }
}

static void theme_apply_colors(void) {
    if (!s_home_scr) return;
    lv_obj_set_style_bg_color(s_home_scr, lv_color_hex(TH.bg), 0);
    if (s_batt) lv_obj_set_style_text_color(s_batt, lv_color_hex(TH.screen_muted), 0);
    if (s_bolt) lv_obj_set_style_line_color(s_bolt, lv_color_hex(TH.screen_ink), 0);
    if (s_clock) lv_obj_set_style_text_color(s_clock, lv_color_hex(TH.screen_ink), 0);
    if (s_ttl) lv_obj_set_style_text_color(s_ttl, lv_color_hex(TH.muted), 0);
    if (s_rev) lv_obj_set_style_text_color(s_rev, lv_color_hex(TH.muted), 0);
    if (s_nick) lv_obj_set_style_text_color(s_nick, lv_color_hex(TH.ink), 0);
    if (s_sign) lv_obj_set_style_text_color(s_sign, lv_color_hex(TH.ink2), 0);
    duo_apply();
    if (s_hero_box) lv_obj_set_style_bg_color(s_hero_box, lv_color_hex(TH.card), 0);
    layout_apply();
    avatar_build();   // 证件照配色跟随主题
}

// dir=+1 下一套，-1 上一套；立即落盘并立即重绘（不做中间帧）。
static void theme_step(int dir) {
    const int64_t now = esp_timer_get_time();
    if (now - s_theme_key_us < 220000) return;
    s_theme_key_us = now;
    s_theme_idx = (uint8_t)((s_theme_idx + THEME_COUNT + dir) % THEME_COUNT);
    s_profile.theme = s_theme_idx;
    profile_save(&s_profile);   // 先落盘，重启后保持
    theme_set_src();
    theme_apply_colors();
    avatar_apply();
    /* 切主题/设置静音：不触发任何语音 */
}

static void build_home(void) {
    s_home_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_home_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_home_scr, 0, 0);
    lv_obj_set_style_border_width(s_home_scr, 0, 0);
    lv_obj_set_style_bg_color(s_home_scr, lv_color_hex(TH.bg), 0);
    lv_obj_set_style_bg_opa(s_home_scr, LV_OPA_COVER, 0);

    // 整卡底图：边框、切角、衬边、文字牌、分隔线、装饰全部在卡面里，固件不重绘。
    s_theme_bg = lv_image_create(s_home_scr);
    lv_obj_set_pos(s_theme_bg, 0, 0);
    theme_set_src();

    // 顶部状态行：时间（左）+ 电量（右）。
    s_clock = mk_text(s_home_scr, "--:--", &lv_font_montserrat_22, TH.screen_ink);
    lv_obj_set_pos(s_clock, HOME_TIME_X, HOME_TIME_Y - 2);
    s_batt = mk_text(s_home_scr, "--%", &lv_font_montserrat_12, TH.screen_muted);
    lv_obj_align(s_batt, LV_ALIGN_TOP_RIGHT, -22, HOME_BATT_Y);
    static const lv_point_precise_t BOLT_PTS[] = {
        {3, 0}, {0, 6}, {2, 6}, {0, 11}, {7, 4}, {4, 4},
    };
    s_bolt = lv_line_create(s_home_scr);
    lv_line_set_points(s_bolt, BOLT_PTS, 6);
    lv_obj_set_style_line_width(s_bolt, 2, 0);
    lv_obj_set_style_line_color(s_bolt, lv_color_hex(TH.screen_ink), 0);
    lv_obj_set_pos(s_bolt, 214, 12);
    lv_obj_add_flag(s_bolt, LV_OBJ_FLAG_HIDDEN);

    // 卡面内容容器：整块参与 180ms 位移动效；坐标为 240x320 卡面绝对坐标。
    s_home_body = mk_clear(s_home_scr, 0, 0, THEME_W, THEME_H);

    // 层级：猫窝/猫/头像主视觉 → 头像位 → 文字 → 签名 mark。
    spr_setup();
    s_home_cat = lv_image_create(s_home_body);
    lv_image_set_src(s_home_cat, &s_dsc[SPR_IDLE]);
    lv_obj_set_pos(s_home_cat, HOME_STAGE_X + SPR[SPR_IDLE].ox,
                   HOME_STAGE_Y + SPR[SPR_IDLE].oy);
    // 配件层压在猫之上：身体 → 面部。
    s_home_acc_body = lv_image_create(s_home_body);
    lv_image_set_src(s_home_acc_body, &s_dsc[SPR_ACC_OFFICE]);
    lv_obj_add_flag(s_home_acc_body, LV_OBJ_FLAG_HIDDEN);
    s_home_acc_face = lv_image_create(s_home_body);
    lv_image_set_src(s_home_acc_face, &s_dsc[SPR_ACC_COOL_FACE]);
    lv_obj_add_flag(s_home_acc_face, LV_OBJ_FLAG_HIDDEN);

    // 主视觉 = 主人照片：88x88 放大 2 倍到 176（整数倍，不拉伸不变形）。
    // 画框（themes.bin）外沿 x28..211 / y36..239、2px 描边、外圆角 10；照片居中放进内框：
    // 左右各留 2px，上下各留 14px。圆角取 10 与外框同曲率，避免“框是圆的、照片角更圆”的双弧。
    s_hero_box = mk_clear(s_home_body, 32, 50, 176, 176);
    lv_obj_set_style_radius(s_hero_box, 10, 0);
    lv_obj_set_style_clip_corner(s_hero_box, true, 0);
    lv_obj_set_style_bg_color(s_hero_box, lv_color_hex(TH.card), 0);
    lv_obj_set_style_bg_opa(s_hero_box, LV_OPA_COVER, 0);
    s_hero_img = lv_image_create(s_hero_box);
    lv_image_set_pivot(s_hero_img, 0, 0);
    lv_image_set_scale(s_hero_img, 512);   // 88 → 176
    lv_obj_set_pos(s_hero_img, 0, 0);
    lv_obj_add_flag(s_hero_box, LV_OBJ_FLAG_HIDDEN);

    // 右下角：只有小猫，没有圆框也没有描边。IDLE 帧 134x164 按 88/256 缩小到 46x56，
    // 右缘 232、底边 290（右/下各留 8px），不压 298 的卡面分界与 305 的 MICRO 行。
    s_mini_cat = lv_image_create(s_home_body);
    lv_image_set_src(s_mini_cat, &s_dsc[SPR_IDLE]);
    lv_image_set_pivot(s_mini_cat, 0, 0);
    lv_image_set_scale(s_mini_cat, 88);
    lv_obj_set_pos(s_mini_cat, 186, 234);

    // 姓名 / 签名：24px 与 16px，长文本省略号，绝不压到头像框与卡面关键线。
    s_nick = mk_text(s_home_body, "UNNAMED", &font_cjk_24, TH.ink);
    lv_obj_set_size(s_nick, HOME_LAYOUT[s_theme_idx].name_w, 34);
    lv_label_set_long_mode(s_nick, LV_LABEL_LONG_MODE_DOTS);
    s_sign = mk_text(s_home_body, "", &font_sign_16, TH.ink2);
    // 高度必须 >= 字库行高（霞鹜文楷 16px 为 20），否则 DOTS 会退化成换行，第二行会压到 MICRO 行。
    lv_obj_set_size(s_sign, HOME_LAYOUT[s_theme_idx].sub_w, 22);
    lv_label_set_long_mode(s_sign, LV_LABEL_LONG_MODE_DOTS);

    // MICRO 行：左 AI PASSPORT，右 NO. 01（10px，只在 band 里）。
    s_ttl = mk_text(s_home_body, "AI PASSPORT", &lv_font_montserrat_10, TH.muted);
    s_rev = mk_text(s_home_body, "NO. 01", &lv_font_montserrat_10, TH.muted);

    // 头像 88x88 RGB565A8：默认指 flash 内置形象，上传过才换成堆缓冲。
    s_av_buf = (uint8_t *)avatar_default_slot(s_profile.sex);
    s_av_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_av_dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
    s_av_dsc.header.flags = 0;
    s_av_dsc.header.w = PROFILE_AVATAR_W;
    s_av_dsc.header.h = PROFILE_AVATAR_W;
    s_av_dsc.header.stride = PROFILE_AVATAR_W * 2;
    s_av_dsc.data_size = PROFILE_AVATAR_BYTES;
    s_av_dsc.data = s_av_buf;
    lv_image_set_src(s_hero_img, &s_av_dsc);

    layout_apply();
    theme_apply_colors();
    hero_apply();
}

static void build_pet_scene(void) {
    s_pet_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_pet_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_pet_scr, 0, 0);
    lv_obj_set_style_border_width(s_pet_scr, 0, 0);
    lv_obj_set_style_bg_color(s_pet_scr, lv_color_hex(P_BG), 0);
    lv_obj_set_style_bg_grad_color(s_pet_scr, lv_color_hex(P_BG2), 0);
    lv_obj_set_style_bg_grad_dir(s_pet_scr, LV_GRAD_DIR_VER, 0);

    // 天气/时段背景层（必须最先创建，位于所有 UI 之下）。
    pet_bg_create(s_pet_scr);

    // 底部信息面板：保证雨雪/亮色天空下文案仍有高对比。
    mk_box(s_pet_scr, 0, 214, SCREEN_W, 1, 0x1B3540, 0);
    mk_box(s_pet_scr, 0, 215, SCREEN_W, SCREEN_H - 215, 0x0A1A20, 0);

    s_pet_title = mk_text(s_pet_scr, "LITTLE COMPANION", &lv_font_montserrat_14, P_ACCENT);
    lv_obj_align(s_pet_title, LV_ALIGN_TOP_LEFT, 8, 5);
    s_pet_batt = mk_text(s_pet_scr, "--%", &lv_font_montserrat_14, P_MUTED);
    // 电量贴右缘：起点约 x=198，与标题（右缘约 153）留出空隙；双圆在 y>=40，不在同一行。
    lv_obj_align(s_pet_batt, LV_ALIGN_TOP_RIGHT, -6, 5);

    // 主视觉：180x196 画布，猫占据大半屏。
    s_pet_root = mk_clear(s_pet_scr, 30, 14, ART_W, ART_H);
    s_glow = mk_ring(s_pet_root, 60, 66, 60, 52, P_ACCENT, 2);
    lv_obj_add_flag(s_glow, LV_OBJ_FLAG_HIDDEN);
    s_art = mk_clear(s_pet_root, 0, 0, ART_W, ART_H);
    for (int i = 0; i < SPARKS; i++) {
        s_spark[i] = mk_box(s_pet_root, 88, 90, 4, 4, P_ACCENT, 2);
        lv_obj_add_flag(s_spark[i], LV_OBJ_FLAG_HIDDEN);
    }

    // 层级：猫窝（底）→ 毛线球 → 猫 → 身体配件 → 面部配件。
    spr_setup();
    s_bed = lv_image_create(s_art);
    lv_image_set_src(s_bed, &s_dsc[SPR_BED_UNIVERSAL]);
    lv_obj_set_pos(s_bed, 0, 0);
    s_yarn = lv_image_create(s_art);
    lv_image_set_src(s_yarn, &s_dsc[SPR_YARN_BALL]);
    lv_obj_add_flag(s_yarn, LV_OBJ_FLAG_HIDDEN);
    s_cat = lv_image_create(s_art);
    s_acc_body = lv_image_create(s_art);
    lv_image_set_src(s_acc_body, &s_dsc[SPR_ACC_OFFICE]);
    lv_obj_add_flag(s_acc_body, LV_OBJ_FLAG_HIDDEN);
    s_acc_face = lv_image_create(s_art);
    lv_image_set_src(s_acc_face, &s_dsc[SPR_ACC_COOL_FACE]);
    lv_obj_add_flag(s_acc_face, LV_OBJ_FLAG_HIDDEN);

    // 底部署名（拍立得落款，美术终稿）：合影 68x34 按原始像素 1:1 贴，不加 padding、
    // 不缩放；右侧小字"你 和 猫名"字号 8，左缘 88（距合影右缘 4px）且基线对齐合影底边。
    s_duo_box = mk_clear(s_pet_scr, SIGN_X, SIGN_Y, SIGN_IMG_W, SIGN_IMG_H);
    s_duo_img = lv_image_create(s_duo_box);
    lv_obj_set_pos(s_duo_img, 0, 0);

    // 8px 信息区：名字/环境/提示/按键全部沉到最底部，不占主视觉。
    const int ink_y = baseline_top(&font_cjk_8, SIGN_NAME_BASE_Y);
    s_duo_text = mk_text(s_pet_scr, "", &font_cjk_8, SIGN_INK);
    lv_obj_set_pos(s_duo_text, SIGN_NAME_X, ink_y);
    s_pet_env = mk_text(s_pet_scr, "", &font_cjk_8, P_MUTED);
    lv_obj_align(s_pet_env, LV_ALIGN_TOP_RIGHT, -(SCREEN_W - SIGN_HINT_RIGHT), ink_y);
    s_pet_hint = mk_text(s_pet_scr, "", &font_cjk_8, P_MUTED);
    lv_obj_align(s_pet_hint, LV_ALIGN_TOP_RIGHT, -(SCREEN_W - SIGN_HINT_RIGHT),
                 baseline_top(&font_cjk_8, SIGN_HINT_BASE_Y));
    s_pet_sub = mk_text(s_pet_scr, "", &font_cjk_8, P_MUTED);
    lv_obj_set_pos(s_pet_sub, SIGN_KEY_L_X, SIGN_KEY_Y);
    s_pet_key = mk_text(s_pet_scr, "", &font_cjk_8, P_MUTED);
    lv_obj_align(s_pet_key, LV_ALIGN_TOP_RIGHT, -(SCREEN_W - SIGN_KEY_R_RIGHT), SIGN_KEY_Y);

    duo_apply();
    bubble_build(s_pet_scr);

    // 领养纪念铭牌：只在拉近观看态出现，压在画面下方。
    s_plate_box = mk_box(s_pet_scr, 18, 228, 204, 66, 0x0A1A20, 14);
    lv_obj_set_style_border_width(s_plate_box, 1, 0);
    lv_obj_set_style_border_color(s_plate_box, lv_color_hex(P_ACCENT), 0);
    s_plate_l1 = mk_text(s_plate_box, "", &font_cjk_16, P_INK);
    lv_obj_align(s_plate_l1, LV_ALIGN_TOP_MID, 0, 10);
    s_plate_l2 = mk_text(s_plate_box, "", &font_cjk_16, P_MUTED);
    lv_obj_align(s_plate_l2, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_add_flag(s_plate_box, LV_OBJ_FLAG_HIDDEN);

    s_pet_toast_box = mk_box(s_pet_scr, 20, 160, 200, 30, 0x163A34, 15);
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

    mk_box(s_set_scr, 16, 14, 6, 6, T_ACCENT, 0);
    lv_obj_t *title = mk_text(s_set_scr, "SETTINGS", &lv_font_montserrat_14, T_INK);
    lv_obj_set_pos(title, 28, 11);
    mk_box(s_set_scr, 16, 32, 208, 1, T_LINE, 0);

    for (int i = 0; i < SET_ROWS; i++) {
        const int y = 44 + i * 32;
        s_set_cursor[i] = mk_box(s_set_scr, 8, y + 4, 4, 18, T_ACCENT, 2);
        s_set_name[i] = mk_text(s_set_scr, SET_NAME(i),
                                s_profile.lang ? &lv_font_montserrat_14 : &font_cjk_16,
                                T_MUTED);
        lv_obj_set_pos(s_set_name[i], 20, y + 5);
        s_set_val[i] = mk_text(s_set_scr, "", &lv_font_montserrat_20, T_INK);
        lv_obj_align(s_set_val[i], LV_ALIGN_TOP_RIGHT, -16, y);
    }

    s_set_toast_box = mk_box(s_set_scr, 20, 270, 200, 28, T_CARD, 14);
    lv_obj_set_style_border_width(s_set_toast_box, 2, 0);
    lv_obj_set_style_border_color(s_set_toast_box, lv_color_hex(T_BEST), 0);
    s_set_toast = mk_text(s_set_toast_box, "", &lv_font_montserrat_14, T_INK);
    lv_obj_center(s_set_toast);
    lv_obj_add_flag(s_set_toast_box, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *hint = mk_text(s_set_scr, "OK CHANGE      HOLD OK EXIT",
                             &lv_font_montserrat_14, T_INK);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 300);
    s_set_hint = hint;
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

    s_portal_hint = mk_text(s_portal_scr, TR("OK 退出", "OK EXIT"),
                            s_profile.lang ? &lv_font_montserrat_14 : &font_cjk_16, T_INK);
    lv_obj_align(s_portal_hint, LV_ALIGN_TOP_MID, 0, 298);
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
    s_low_power = false;
    bsp_display_backlight(s_profile.bright ? s_profile.bright : 100);
    ESP_LOGI(TAG, "PWR wake from dim view=%d", (int)s_view);
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
    // 流畅度统计：真实屏幕刷新次数由 LVGL 渲染完成事件累加，
    // 同时记录每次 ui_tick 的耗时上界与 LVGL 池最低余量。
    int64_t perf_us = last_us;
    uint32_t perf_frames = 0;
    int64_t perf_tick_sum = 0;
    uint32_t perf_tick_n = 0;
    int64_t perf_tick_max = 0;
    uint32_t perf_free_min = 0xFFFFFFFFu;

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
            // LVGL 对象必须在锁内操作：wake_screen 会进入 VIEW_HOME 并改动对象。
            if (woke && bsp_lvgl_lock(200)) {
                wake_screen();
                bsp_lvgl_unlock();
            }
            continue;
        }

        // 省电态：任意按键只负责唤醒（恢复亮度 + 动画），这一次按键不当作互动。
        if (s_low_power) {
            input_ev_t drop;
            bool woke = false;
            while (s_input_q && xQueueReceive(s_input_q, &drop, 0) == pdTRUE) woke = true;
            if (woke) {
                s_low_power = false;
                bsp_display_backlight(s_profile.bright ? s_profile.bright : 100);
                ESP_LOGI(TAG, "PWR wake after %lldms idle bob=%ld frame=%u lift=%d",
                         (long long)((esp_timer_get_time() - s_last_act_us) / 1000),
                         (long)s_bob_ms, (unsigned)s_frame, (int)s_lift);
                s_last_act_us = esp_timer_get_time();
            }
        }

        if (bsp_lvgl_lock(200)) {
            input_ev_t in;
            while (s_input_q && xQueueReceive(s_input_q, &in, 0) == pdTRUE) {
                if (s_lat_on) {
                    s_lat_key_ms = lv_tick_get();
                    s_lat_pending = true;
                }
                handle_key(in.btn, in.ev);
            }
            bsp_lvgl_unlock();
        }

        if (s_need_save) {
            s_need_save = false;
            (void)pet_store_save(&s_pet);
        }

        // 电量采样会更新标签文字/颜色/图标，必须持有 LVGL 锁。
        if (++frame % 30 == 0 && bsp_lvgl_lock(200)) {
            sample_battery();
            bsp_lvgl_unlock();
        }

        // 门户开启时不息屏（3 分钟无请求由门户自己超时关闭）。
        const bool portal_open = (s_view == VIEW_PORTAL);
        const uint8_t idle_sec = s_profile.idle;
        if (!portal_open && idle_sec > 0) {
            const int64_t idle_us = now_us - s_last_act_us;
            // 息屏：沿用原有逻辑（背光 5）。
            if (idle_us >= (int64_t)idle_sec * 1000000LL) {
                s_dimmed = true;
                bsp_display_backlight(5);
                continue;
            }
            // 省电：无操作到阈值一半先把背光压到最低并冻结待机动画；
            // 不新增定时器，复用上面同一个 idle 计时和 s_last_act_us。
            if (!s_low_power && idle_us >= (int64_t)idle_sec * 500000LL) {
                s_low_power = true;
                bsp_display_backlight(BRIGHT_MIN);
                ESP_LOGI(TAG, "PWR low idle=%lldms bl=%d view=%d bob=%ld frame=%u lift=%d",
                         (long long)(idle_us / 1000), BRIGHT_MIN, (int)s_view,
                         (long)s_bob_ms, (unsigned)s_frame, (int)s_lift);
            }
        }

        if (bsp_lvgl_lock(200)) {
            const int64_t t0 = esp_timer_get_time();
            ui_tick(dt);
            const int64_t used = esp_timer_get_time() - t0;
            lv_mem_monitor_t mon;
            lv_mem_monitor(&mon);
            if (mon.free_size < perf_free_min) perf_free_min = mon.free_size;
            bsp_lvgl_unlock();
            perf_tick_sum += used;
            perf_tick_n++;
            if (used > perf_tick_max) perf_tick_max = used;
        }

        if (now_us - perf_us >= 5000000) {
            const int32_t secs = (int32_t)((now_us - perf_us) / 1000000);
            lv_mem_monitor_t mon;
            lv_mem_monitor(&mon);
            ESP_LOGI(TAG,
                     "PERF fps=%.1f tick_avg=%dms tick_max=%dms pool_used=%uB "
                     "pool_max=%uB pool_free=%uB frag=%u%% heap=%uB largest=%uB",
                     (double)(s_render_cnt - perf_frames) / (secs ? secs : 1),
                     perf_tick_n ? (int)(perf_tick_sum / perf_tick_n / 1000) : 0,
                     (int)(perf_tick_max / 1000),
                     (unsigned)(mon.total_size - mon.free_size),
                     (unsigned)mon.max_used, (unsigned)mon.free_size,
                     (unsigned)mon.frag_pct, (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT |
                                                                MALLOC_CAP_INTERNAL));
            perf_us = now_us;
            perf_frames = s_render_cnt;
            perf_tick_sum = 0;
            perf_tick_n = 0;
            perf_tick_max = 0;
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
    // 补写首次领养时间：用本次开机时长反推，保证不晚于当前时间、不出现未来日期。
    if (s_adopt_pending || s_pet.adopt_utc == 0) {
        const int64_t up = (s_adopt_boot_us >= 0)
                               ? (esp_timer_get_time() - s_adopt_boot_us) / 1000000
                               : 0;
        int64_t adopt = epoch - up;
        if (adopt < 1) adopt = 1;
        if (adopt > epoch) adopt = epoch;
        s_pet.adopt_utc = (uint64_t)adopt;
        s_adopt_pending = false;
        s_need_save = true;
    }
    ESP_LOGI(TAG, "NTP 校时: epoch=%lld", (long long)epoch);
    if (s_home_scr && bsp_lvgl_lock(200)) {
        refresh_home();
        bsp_lvgl_unlock();
    }
}

// 联调/测试入口：串口 "FAP_TEST <sub>"。只有被调用时才生效，正常使用零开销。
void pet_app_test_cmd(const char *args) {
    if (!args || !*args) return;
    if (strncmp(args, "lat", 3) == 0) {
        s_lat_on = true;
        ESP_LOGI(TAG, "TEST lat on");
    } else if (strncmp(args, "keys", 4) == 0) {
        if (!bsp_lvgl_lock(1000)) return;
        s_test_keys = atoi(args + 5);
        if (s_test_keys <= 0) s_test_keys = 300;
        s_test_next_ms = lv_tick_get() + 150;
        bsp_lvgl_unlock();
        ESP_LOGI(TAG, "TEST keys start n=%d", s_test_keys);
    } else if (strcmp(args, "keyok") == 0) {
        // 真机验证钩子：一次 OK 短按塞进输入队列，走和物理按键完全相同的路径。
        if (s_input_q) {
            const input_ev_t e = {.btn = BSP_BTN_OK, .ev = BSP_BTN_CLICK};
            (void)xQueueSend(s_input_q, &e, 0);
            ESP_LOGI(TAG, "TEST keyok queued");
        }
    } else if (strcmp(args, "avold") == 0) {
        // 复现旧档：往 passport/ava 写一块 2,192 B（旧 44 索引色头像同长度）垃圾数据，
        // 再走正常重建路径，看固件是否把它当“无头像”回落内置形象。
        // 临时块走堆：主任务栈只有 3,584 B，不能放栈上。
        const size_t old_len = 2192;
        uint8_t *old_av = malloc(old_len);
        if (!old_av) {
            ESP_LOGE(TAG, "TEST avold: no mem");
            return;
        }
        memset(old_av, 0x5A, old_len);
        nvs_handle_t h;
        esp_err_t e = nvs_open("passport", NVS_READWRITE, &h);
        if (e == ESP_OK) {
            e = nvs_set_blob(h, "ava", old_av, old_len);
            if (e == ESP_OK) e = nvs_commit(h);
            nvs_close(h);
        }
        free(old_av);
        ESP_LOGI(TAG, "TEST avold write=%s len=%u", esp_err_to_name(e),
                 (unsigned)old_len);
        if (bsp_lvgl_lock(1000)) {
            avatar_apply();
            bsp_lvgl_unlock();
        }
    } else if (strcmp(args, "avinfo") == 0) {
        nvs_handle_t h;
        size_t len = 0;
        esp_err_t e = nvs_open("passport", NVS_READONLY, &h);
        if (e == ESP_OK) {
            e = nvs_get_blob(h, "ava", NULL, &len);
            nvs_close(h);
        }
        ESP_LOGI(TAG, "TEST avinfo get=%s len=%u new=%u custom=%d",
                 esp_err_to_name(e), (unsigned)len, (unsigned)PROFILE_AVATAR_BYTES,
                 (int)profile_avatar_custom());
    } else if (strncmp(args, "theme20", 7) == 0) {
        if (!bsp_lvgl_lock(1000)) return;
        s_test_theme = 20;
        s_test_next_ms = lv_tick_get() + 100;
        bsp_lvgl_unlock();
        ESP_LOGI(TAG, "TEST theme20 start");
    } else if (strncmp(args, "audio20", 7) == 0) {
        if (!bsp_lvgl_lock(1000)) return;
        s_test_audio = 20;
        s_test_next_ms = lv_tick_get() + 100;
        bsp_lvgl_unlock();
        ESP_LOGI(TAG, "TEST audio20 start");
    } else if (strncmp(args, "bench ", 6) == 0) {
        if (!bsp_lvgl_lock(1000)) return;
        snprintf(s_bench_tag, sizeof(s_bench_tag), "%s", args + 6);
        s_bench_f0 = s_render_cnt;
        s_bench_tick0 = s_tick_cnt;
        s_bench_t0 = lv_tick_get();
        s_bench_sec = 6;
        s_bench_on = true;
        if (strcmp(s_bench_tag, "anim") == 0) {
            s_test_anim = 190;               // 每 tick 换帧，覆盖 6s
            s_test_anim_next = lv_tick_get();
        }
        bsp_lvgl_unlock();
        ESP_LOGI(TAG, "BENCH start tag=%s", s_bench_tag);
    } else if (strncmp(args, "soak", 4) == 0) {
        int sec = atoi(args + 5);
        if (sec <= 0) sec = 1800;
        s_soak_heap_min = 0xFFFFFFFFu;
        s_soak_end_ms = lv_tick_get() + (uint32_t)sec * 1000u;
        ESP_LOGI(TAG, "TEST soak start sec=%d", sec);
    } else if (strncmp(args, "clock ", 6) == 0) {
        pet_app_set_epoch((int64_t)atoll(args + 6));
    } else if (strncmp(args, "navdump", 7) == 0) {
        profile_t p;
        profile_load(&p);
        pet_state_t ps;
        pet_store_load(&ps, NULL);
        ESP_LOGI(TAG,
                 "NVS sex=%u acc=%u hero=%u theme=%u vol=%u bright=%u idle=%u "
                 "growth=%lu mood=%u adopt=%llu",
                 p.sex, p.acc, p.hero, p.theme, p.volume, p.bright, p.idle,
                 (unsigned long)ps.growth, (unsigned)ps.mood,
                 (unsigned long long)ps.adopt_utc);
    } else if (strcmp(args, "nvsdump") == 0) {
        // NVS 余量实测：分区条目统计 + 两个命名空间各自占用。
        nvs_stats_t st;
        if (nvs_get_stats(NULL, &st) == ESP_OK) {
            ESP_LOGI(TAG, "NVS total=%u used=%u free=%u ns=%u",
                     (unsigned)st.total_entries, (unsigned)st.used_entries,
                     (unsigned)st.free_entries, (unsigned)st.namespace_count);
        }
        const char *nsv[] = {"passport", "pet"};
        for (int i = 0; i < 2; i++) {
            nvs_handle_t h;
            if (nvs_open(nsv[i], NVS_READONLY, &h) != ESP_OK) {
                ESP_LOGW(TAG, "NVS ns=%s missing", nsv[i]);
                continue;
            }
            size_t used = 0;
            if (nvs_get_used_entry_count(h, &used) == ESP_OK) {
                ESP_LOGI(TAG, "NVS ns=%s used_entries=%u", nsv[i], (unsigned)used);
            }
            nvs_close(h);
        }
    } else if (strcmp(args, "nvsbench") == 0) {
        // 10,240 B blob 写入耗时实测：写临时键后立刻删除，不留档。
        const size_t n = 10240;
        uint8_t *blob = malloc(n);
        if (!blob) {
            ESP_LOGW(TAG, "NVS bench no mem");
            return;
        }
        memset(blob, 0x5A, n);
        nvs_handle_t h;
        if (nvs_open("passport", NVS_READWRITE, &h) == ESP_OK) {
            int64_t t0 = esp_timer_get_time();
            esp_err_t e1 = nvs_set_blob(h, "avtmp", blob, n);
            int64_t t1 = esp_timer_get_time();
            esp_err_t e2 = nvs_commit(h);
            int64_t t2 = esp_timer_get_time();
            esp_err_t e3 = nvs_erase_key(h, "avtmp");
            esp_err_t e4 = nvs_commit(h);
            int64_t t3 = esp_timer_get_time();
            nvs_close(h);
            ESP_LOGI(TAG, "NVS bench set=%d %lldus commit=%d %lldus erase=%d/%d %lldus",
                     e1, (long long)(t1 - t0), e2, (long long)(t2 - t1),
                     e3, e4, (long long)(t3 - t2));
        }
        free(blob);
    } else if (strcmp(args, "setsave") == 0) {
        // 存档持久化用例：一次改完性别/配件/主视觉/主题并落盘，重启后核对。
        s_profile.sex = 1;
        s_profile.acc = 3;
        s_profile.hero = 1;
        s_profile.theme = 2;
        profile_save(&s_profile);
        ESP_LOGI(TAG, "TEST setsave sex=1 acc=3 hero=1 theme=2");
    } else if (strncmp(args, "oldblob", 7) == 0) {
        ESP_LOGI(TAG, "TEST oldblob write=%d", (int)pet_store_test_old_blob());
    } else {
        ESP_LOGW(TAG, "TEST unknown: %s", args);
    }
}

void pet_app_refresh(void) {
    const uint8_t old_outfit = s_profile.outfit;
    profile_load(&s_profile);
    s_profile.hero = HERO_AVATAR;   // 主视觉固定证件照，NVS 旧 hero 字段不再参与
    bsp_audio_set_volume(s_profile.volume);
    if (!s_home_scr) return;
    if (bsp_lvgl_lock(200)) {
        if (old_outfit != s_profile.outfit) {
            draw_pet();
        }
        avatar_apply();
        refresh_home();
        bsp_lvgl_unlock();
    }
}

void pet_app_shot_scene(const char *name) {
    ESP_LOGI(TAG, "SHOT begin %s scr=%p", name ? name : "(null)", (void *)s_pet_scr);
    if (!s_pet_scr) { ESP_LOGW(TAG, "SHOT no scr"); return; }
    if (!bsp_lvgl_lock(3000)) { ESP_LOGW(TAG, "SHOT lock timeout"); return; }
    if (!name || strcmp(name, "home") == 0) {
        enter_view(VIEW_HOME);
        s_home_in_ms = -1;
        if (s_home_body) lv_obj_set_pos(s_home_body, 0, 0);
    } else if (strcmp(name, "zh") == 0) {
        // 仅用于截图验证：临时写入中文昵称/签名（不落 NVS）。
        snprintf(s_profile.nick, sizeof(s_profile.nick), "林小满");
        // 覆盖新增字模：噜 + 全角括号（）；短句保证括号落在省略号之前，能看见。
        snprintf(s_profile.sign, sizeof(s_profile.sign), "咕噜咕噜（喵）");
        refresh_home();
        enter_view(VIEW_HOME);
        // 出图不等入场动画，否则会截到滑入中间帧，底部文字像被裁。
        s_home_in_ms = -1;
        if (s_home_body) lv_obj_set_pos(s_home_body, 0, 0);
    } else if (strcmp(name, "themesave") == 0 || strcmp(name, "themereset") == 0) {
        // 持久化验证钩子：写入指定主题并落盘，随后复位设备检查是否保持。
        s_theme_idx = (strcmp(name, "themesave") == 0) ? (THEME_COUNT - 1) : 0;
        s_profile.theme = s_theme_idx;
        profile_save(&s_profile);
        enter_view(VIEW_HOME);
        theme_set_src();
        theme_apply_colors();
        avatar_apply();
        if (s_theme_bg) lv_obj_set_style_opa(s_theme_bg, LV_OPA_COVER, 0);
        // 出图不等入场动画：直接归位，避免截到偏移中的中间帧。
        s_home_in_ms = -1;
        if (s_home_body) lv_obj_set_pos(s_home_body, 0, 0);
    } else if (strcmp(name, "portal") == 0) {
        // 实机验证钩子：走设置页同一条异步路径，避免卡死取帧任务。
        enter_view(VIEW_SET);
        portal_open_async();
    } else if (strcmp(name, "sex0") == 0 || strcmp(name, "sex1") == 0) {
        // 截图钩子：切主人形象（不落 NVS），验证头像框即时刷新。
        s_profile.sex = (name[3] == '1') ? 1 : 0;
        avatar_apply();
        enter_view(VIEW_HOME);
    } else if (strcmp(name, "hero") == 0 || strcmp(name, "heroc") == 0) {
        // 截图钩子：主视觉 = 头像 / 猫（不落 NVS）。
        s_profile.hero = (strcmp(name, "hero") == 0) ? 1 : 0;
        hero_apply();
        enter_view(VIEW_HOME);
    } else if (strncmp(name, "acc", 3) == 0 && name[3] >= '0' && name[3] <= '3' &&
               name[4] == '\0') {
        // 截图钩子：切配件造型（不落 NVS）。0/1 关、2 上班族西装、3 酷外套+墨镜。
        s_profile.acc = (uint8_t)(name[3] - '0');
        acc_apply();
        enter_view(VIEW_HOME);
        home_acc_apply();
        s_home_in_ms = -1;
        if (s_home_body) lv_obj_set_pos(s_home_body, 0, 0);
    } else if (strcmp(name, "set") == 0) {
        s_set_cursor_row = 4;   // 光标停在 THEME 行，截图可见主题名
        settings_refresh();
        enter_view(VIEW_SET);
    } else if (strncmp(name, "theme", 5) == 0 && name[5] >= '0' &&
               name[5] < '0' + THEME_COUNT && name[6] == '\0') {
        // 截图钩子：直接定位到第 N 套主题（不落 NVS）。
        s_theme_idx = (uint8_t)(name[5] - '0');
        enter_view(VIEW_HOME);
        theme_set_src();
        theme_apply_colors();
        avatar_apply();
        if (s_theme_bg) lv_obj_set_style_opa(s_theme_bg, LV_OPA_COVER, 0);
        // 出图不等入场动画：直接归位，避免截到偏移中的中间帧。
        s_home_in_ms = -1;
        if (s_home_body) lv_obj_set_pos(s_home_body, 0, 0);
    } else {
        s_wake_ms = 0;
        s_summoned = true;
        s_anim = ANIM_NONE;
        s_hop = HOP_OFF;
        s_fx = FX_NONE;
        s_fx_ms = 0;
        s_slide = 0;
        s_lift = 0;
        if (strcmp(name, "play") == 0) {
            s_cat_state = CAT_PLAY;
        } else if (strcmp(name, "sleep") == 0) {
            s_cat_state = CAT_SLEEP;
        } else if (strcmp(name, "happy") == 0) {
            s_cat_state = CAT_COZY;
            s_fx = FX_HAPPY;
            s_fx_ms = 60000;
        } else {
            s_cat_state = CAT_COZY;
        }
        if (strncmp(name, "acc", 3) == 0 && name[3] >= '1' && name[3] <= '3') {
            // 截图钩子：直接戴第 N 件配件（1 眼镜 / 2 上班族 / 3 酷，不落 NVS）。
            s_profile.acc = (uint8_t)(name[3] - '0');
        } else {
            s_profile.acc = 0;
        }
        // 先进场景，再摆姿态/配件：enter_view 内部会 refresh_pet_scene，先设会被覆盖。
        s_pose = 0;
        s_pose_ms = 0;
        enter_view(VIEW_PET);
        refresh_pet_scene();
        draw_pet();
        lv_obj_set_pos(s_art, 0, 0);
        lv_obj_set_style_opa(s_art, LV_OPA_COVER, 0);
        // 蹦跳取帧：直接摆到腾空顶点的拉长帧 / 落地压缩帧，静态截图不用等动画。
        if (strcmp(name, "air") == 0) {
            s_slide = -6;
            s_lift = -16;
            apply_frame(SPR_AIR);
        } else if (strcmp(name, "land") == 0) {
            s_slide = 6;
            s_lift = 0;
            apply_frame(SPR_LAND);
        }
        // 截图用静态姿态：打滚 / 玩毛线球。
        if (strcmp(name, "roll") == 0) {
            s_pose = 1;
            s_pose_ms = 60000;
            apply_frame(frame_for_state());
        } else if (strcmp(name, "yarn") == 0) {
            s_pose = 2;
            s_pose_ms = 60000;
            apply_frame(frame_for_state());
        }
        // 领养铭牌截图：固定一个已知领养日，只为出图，不落 NVS。
        // plateno：同一铭牌但保持"未校时"，验证只显示"第 N 天"、不猜日期。
        if (strcmp(name, "plate") == 0 || strcmp(name, "plateno") == 0) {
            if (strcmp(name, "plate") == 0) {
                s_epoch_base = 1793000000;
                s_epoch_anchor_us = esp_timer_get_time();
                s_time_known = true;
            } else {
                s_epoch_base = 0;
                s_time_known = false;
                s_adopt_boot_us = esp_timer_get_time();
            }
            s_pet.adopt_utc = 1748707200ULL;   // 2025-06-01 00:00 (UTC+8)
            zoom_enter();
            lv_refr_now(NULL);
            bsp_lvgl_unlock();
            return;
        }
        // 截图时不做淡入等待：直接把气泡定在完全显示的状态。
        bubble_say(line_for(LY_GREET));
        lv_obj_set_style_opa(s_bub, LV_OPA_COVER, 0);
        s_bub_ms = BUB_FADE_IN;
    }
    // 同步刷一遍再取帧：切场景后的首帧可能还没画完，避免截到上一屏或半成品。
    lv_refr_now(NULL);
    bsp_lvgl_unlock();
    ESP_LOGI(TAG, "SHOT ready %s", name ? name : "(null)");
}

static const bool cap_enabled = true; // 串口取帧总开关

void pet_app_start(void) {
    profile_load(&s_profile);
    // 设置页第 6 行已从「主视觉」换成「形象」：首页固定显示证件照（上传图 > 内置 M/F）。
    s_profile.hero = HERO_AVATAR;
    s_theme_idx = s_profile.theme < THEME_COUNT ? s_profile.theme : 0;
    pet_load_info_t info;
    pet_store_load(&s_pet, &info);
    // 首次领养：旧档没有该字段时补写一次（不清档）；未校时先记运行锚点，校时后落盘。
    if (s_pet.adopt_utc == 0) {
        s_adopt_pending = true;
        s_adopt_boot_us = esp_timer_get_time();
    }
    s_last_act_us = esp_timer_get_time();

    s_input_q = xQueueCreate(16, sizeof(input_ev_t));
    s_snd_q = xQueueCreate(2, sizeof(uint32_t));
    if (!s_input_q || !s_snd_q) {
        ESP_LOGE(TAG, "输入/音频队列创建失败");
        return;
    }

    if (bsp_lvgl_lock(1000)) {
        lv_display_add_event_cb(lv_display_get_default(), on_render_ready,
                                LV_EVENT_RENDER_READY, NULL);
        build_home();
        build_pet_scene();
        build_settings();
        build_portal();
        apply_lang();   // 按 NVS 里的 lang 落地中英两套界面文案
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
    // 串口取帧（发布助手 FAP_SCREENSHOT_V1），只读观测，不影响运行。
    if (cap_enabled) screen_capture_start();

    const esp_err_t btn = bsp_button_init(on_key, NULL);
    if (btn != ESP_OK) ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(btn));

    // 4096 在连续触发叫声时会栈溢出（实测 panic: Stack protection fault），抬到 8192。
    if (xTaskCreate(audio_task, "pet_audio", 8192, NULL, 12, NULL) != pdPASS) {
        ESP_LOGW(TAG, "音频任务创建失败，静音运行");
    }
    if (xTaskCreate(app_task, "pet_app", 6144, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "主循环任务创建失败");
    }

    if (!info.had_valid) (void)pet_store_save(&s_pet);

    ESP_LOGI(TAG, "宠物养成启动: growth=%lu stage=%u",
             (unsigned long)s_pet.growth, (unsigned)s_pet.stage);
}
