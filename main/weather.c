// main/weather.c
// 用 wttr.in 的 `?format=%C` 取当前天气描述（约 20 字节），映射成场景枚举。
// 走 HTTP 而不是 HTTPS：设备无 RTC，TLS 证书校验依赖正确时间；HTTP 实测可直接 200。
#include "weather.h"

#include <ctype.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_portal.h"

static const char *TAG = "weather";

#define WEATHER_URL "http://wttr.in/?format=%C"
#define REFRESH_US (30LL * 60 * 1000000)
#define RETRY_US (2LL * 60 * 1000000)
#define VALID_US (2LL * 60 * 60 * 1000000)
#define RESP_MAX 64

static volatile wx_kind_t s_kind = WX_UNKNOWN;
static volatile int64_t s_valid_until_us;

static bool contains_ci(const char *hay, const char *needle) {
    const size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == n) return true;
    }
    return false;
}

static wx_kind_t classify(const char *text) {
    // 顺序重要：先判降水，"Partly cloudy" 归多云。
    if (contains_ci(text, "thunder") || contains_ci(text, "storm")) return WX_THUNDER;
    if (contains_ci(text, "snow") || contains_ci(text, "sleet") ||
        contains_ci(text, "blizzard")) {
        return WX_SNOW;
    }
    if (contains_ci(text, "rain") || contains_ci(text, "drizzle") ||
        contains_ci(text, "shower")) {
        return WX_RAIN;
    }
    if (contains_ci(text, "overcast")) return WX_OVERCAST;
    if (contains_ci(text, "fog") || contains_ci(text, "mist") ||
        contains_ci(text, "haze")) {
        return WX_FOG;
    }
    if (contains_ci(text, "cloud")) return WX_CLOUDY;
    if (contains_ci(text, "sunny") || contains_ci(text, "clear")) return WX_SUNNY;
    return WX_UNKNOWN;
}

// 返回 true 表示这次拿到了可用的完整响应。
static bool fetch_once(char *buf, size_t cap) {
    esp_http_client_config_t cfg = {
        .url = WEATHER_URL,
        .timeout_ms = 8000,
        .buffer_size = 64,
        // wttr.in 的 HTTP 入口直接 200；不跟随跳转，避免无 RTC 时的 TLS 校验。
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return false;

    esp_http_client_set_header(cli, "User-Agent", "ai-passport/1.1");
    bool ok = false;
    const esp_err_t err = esp_http_client_open(cli, 0);
    if (err == ESP_OK) {
        const int content_len = esp_http_client_fetch_headers(cli);
        const int status = esp_http_client_get_status_code(cli);
        if (status == 200 && content_len < (int)cap) {
            const int n = esp_http_client_read(cli, buf, (int)cap - 1);
            if (n > 0) {
                buf[n] = 0;
                ok = true;
            }
        } else {
            ESP_LOGW(TAG, "HTTP status=%d len=%d", status, content_len);
        }
        esp_http_client_close(cli);
    } else {
        ESP_LOGW(TAG, "连接 wttr.in 失败: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(cli);
    return ok;
}

static void weather_task(void *arg) {
    (void)arg;
    char buf[RESP_MAX];

    // 没有配网时每 5 秒看一次，配网成功后再拉取。
    while (!wifi_portal_connected()) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    for (;;) {
        int64_t wait_us = RETRY_US;
        if (wifi_portal_connected() && fetch_once(buf, sizeof(buf))) {
            const wx_kind_t kind = classify(buf);
            // 去掉尾部换行，日志里看得清。
            for (char *p = buf; *p; p++) {
                if (*p == '\r' || *p == '\n') *p = 0;
            }
            if (kind != WX_UNKNOWN) {
                s_kind = kind;
                s_valid_until_us = esp_timer_get_time() + VALID_US;
                wait_us = REFRESH_US;
                ESP_LOGI(TAG, "天气: %s (%s)", weather_name(kind), buf);
            } else {
                ESP_LOGW(TAG, "无法识别天气描述: %s", buf);
            }
        }
        vTaskDelay(pdMS_TO_TICKS((uint32_t)(wait_us / 1000)));
    }
}

void weather_start(void) {
    if (xTaskCreate(weather_task, "weather", 5120, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "天气任务创建失败，背景回退默认");
    }
}

wx_kind_t weather_kind(void) {
    if (esp_timer_get_time() > s_valid_until_us) return WX_UNKNOWN;
    return s_kind;
}

const char *weather_name(wx_kind_t kind) {
    switch (kind) {
    case WX_SUNNY: return "SUNNY";
    case WX_CLOUDY: return "CLOUDY";
    case WX_OVERCAST: return "OVERCAST";
    case WX_RAIN: return "RAIN";
    case WX_SNOW: return "SNOW";
    case WX_FOG: return "FOG";
    case WX_THUNDER: return "STORM";
    default: return "--";
    }
}
