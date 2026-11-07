// main/wifi_portal.h
// STA 常驻联网（NTP 授时）+ 按需开启的 SoftAP 自定义门户。
// 手机连热点后访问 http://192.168.4.1 改昵称/签名/上传 Q 版头像。
#pragma once

#include <stdbool.h>
#include <stddef.h>

// 在 app_main 里、UI 启动之后调用：初始化 WiFi（仅 STA，自动重连），NTP 后台任务。
void wifi_portal_start(void);

// 按需开启/关闭门户（SoftAP + HTTP）。开启前 3 分钟无 HTTP 请求会自动视为空闲。
bool wifi_portal_open(void);
void wifi_portal_close(void);
bool wifi_portal_is_open(void);

// 距最近一次 HTTP 请求是否已超过 180 秒（门户空闲超时）。
bool wifi_portal_expired(void);

// 最近一次 HTTP 请求的时间戳（esp_timer_get_time 基准），供 UI 显示剩余时间。
long long wifi_portal_last_req_us(void);

// 热点信息（门户开启后有效）。密码运行时由 MAC 派生，只显示在屏幕上。
const char *wifi_portal_ssid(void);
const char *wifi_portal_pass(void);

// 是否已连上路由器（STA）。
bool wifi_portal_connected(void);
