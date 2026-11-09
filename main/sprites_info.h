// sprites_info.h —— 由 pack_v110.py 自动生成，请勿手改。
#pragma once
#include <stdint.h>

#define SPR_COUNT 26
#define SPR_FRAME_COUNT 10
#define SPR_FRAME_IDLE SPR_IDLE

// 配件锚点：脸部图按 idle 鼻梁/瞳距标定，逐帧映射沿用旧眼镜的鼻梁锚点算法。
#define ACC_REF_NOSE_X 79
#define ACC_REF_NOSE_Y 102
#define ACC_REF_EYE_D 49
#define ACC_REF_EYE_ANG 12
#define ACC_GLASS_BRIDGE_X 52
#define ACC_GLASS_BRIDGE_Y 26
#define ACC_COOL_BRIDGE_X 49
#define ACC_COOL_BRIDGE_Y 22

enum {
    SPR_CAT_IDLE,
    SPR_CAT_CURIOUS,
    SPR_CAT_SLEEPY,
    SPR_CAT_HAPPY,
    SPR_CAT_AIR,
    SPR_CAT_LAND,
    SPR_CAT_SLEEP,
    SPR_CAT_POUNCE,
    SPR_POSE_ROLL,
    SPR_POSE_YARN,
    SPR_BED_UNIVERSAL,
    SPR_YARN_BALL,
    SPR_ACC_GLASSES,
    SPR_ACC_COOL_FACE,
    SPR_ACC_COOL_BODY,
    SPR_ACC_OFFICE,
    SPR_AVATAR_M,
    SPR_AVATAR_M_36,
    SPR_AVATAR_F,
    SPR_AVATAR_F_36,
    SPR_CAT_HEAD,
    SPR_SIGN_F,
    SPR_SIGN_M,
    SPR_AVATAR46_F,
    SPR_AVATAR46_M,
    SPR_CAT_CLOSE
};

// 短名别名：业务代码沿用旧命名（SPR_IDLE / SPR_SLEEP / SPR_AIR…）。
#define SPR_IDLE    SPR_CAT_IDLE
#define SPR_CURIOUS SPR_CAT_CURIOUS
#define SPR_SLEEPY  SPR_CAT_SLEEPY
#define SPR_HAPPY   SPR_CAT_HAPPY
#define SPR_AIR     SPR_CAT_AIR
#define SPR_LAND    SPR_CAT_LAND
#define SPR_SLEEP   SPR_CAT_SLEEP
#define SPR_POUNCE  SPR_CAT_POUNCE
#define SPR_CLOSE   SPR_CAT_CLOSE

typedef struct {
    uint32_t off;   // sprites.bin 内偏移（RGB565A8：先 RGB565 平面，再 A8 平面）
    uint16_t w, h;
    int16_t ox, oy;   // 在 180x196 画布内的左上角
    int16_t eye_x, eye_y;  // 两眼连线中点（相对图像左上角），逐帧标定
    int16_t nose_x, nose_y;// 鼻梁点（相对图像左上角）：镜桥必须落在这里
    uint16_t eye_d;        // 两眼瞳孔间距（像素），决定眼镜缩放
    int16_t eye_ang;       // 两眼连线角度（0.1 度，顺时针为正），决定眼镜旋转
    uint8_t face;          // 1=侧脸/眯眼帧：不画面部配件
    uint8_t kind;          // 0=猫帧 1=猫窝 2=脸部配件 3=身体配件 4=道具
} spr_info_t;

static const spr_info_t SPR[SPR_COUNT] = {
    {0, 134, 164, 22, 26, 60, 72, 57, 76, 49, 12, 0, 0},
    {65928, 139, 166, 20, 24, 49, 78, 47, 83, 48, -24, 0, 0},
    {135152, 139, 153, 19, 37, 58, 90, 62, 95, 52, -132, 0, 0},
    {198956, 158, 177, 12, 12, 52, 89, 52, 96, 48, -24, 0, 0},
    {282856, 114, 164, 32, 9, 78, 38, 80, 41, 33, 17, 0, 0},
    {338944, 168, 117, 4, 72, 87, 88, 86, 94, 48, -12, 0, 0},
    {397912, 138, 124, 21, 67, 66, 86, 66, 90, 44, 13, 0, 0},
    {449248, 165, 151, 7, 40, 52, 78, 51, 81, 40, 14, 0, 0},
    {523996, 166, 166, 7, 24, 0, 0, 0, 0, 0, 0, 1, 0},
    {606664, 145, 109, 17, 81, 0, 0, 0, 0, 0, 0, 1, 0},
    {654080, 180, 196, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {759920, 73, 46, 107, 150, 0, 0, 0, 0, 0, 0, 0, 4},
    {769996, 126, 39, 27, 76, 0, 0, 0, 0, 0, 0, 0, 2},
    {784740, 120, 27, 30, 80, 0, 0, 0, 0, 0, 0, 0, 2},
    {794460, 121, 58, 30, 123, 0, 0, 0, 0, 0, 0, 0, 3},
    {815516, 139, 71, 0, 125, 0, 0, 0, 0, 0, 0, 0, 3},
    {845124, 96, 96, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5},
    {872772, 36, 36, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5},
    {876660, 96, 96, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5},
    {904308, 36, 36, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5},
    {908196, 34, 34, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5},
    {911664, 68, 34, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6},
    {918600, 68, 34, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6},
    {925536, 46, 46, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6},
    {931884, 46, 46, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6},
    {938232, 175, 194, 5, 2, 76, 96, 74, 100, 49, 12, 0, 0}
};
