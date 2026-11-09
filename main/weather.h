// main/weather.h
// 当天天气（wttr.in 纯文本接口）：联网后后台拉取，UI 只读枚举。
// 没有 RTC、也没有缓存到 NVS：本次开机没拿到有效结果时返回 WX_UNKNOWN，
// 场景层据此回退到安全的默认背景。
#pragma once

typedef enum {
    WX_UNKNOWN = 0,
    WX_SUNNY,
    WX_CLOUDY,
    WX_OVERCAST,
    WX_RAIN,
    WX_SNOW,
    WX_FOG,
    WX_THUNDER,
} wx_kind_t;

// 创建后台任务（5 KB 栈）。在 wifi_portal_start 之后调用。
void weather_start(void);

// 当前天气。结果超过有效期（2 小时）或从未成功时返回 WX_UNKNOWN。
wx_kind_t weather_kind(void);

// 固定短名（Montserrat 14 可显示的全大写 ASCII）。
const char *weather_name(wx_kind_t kind);
