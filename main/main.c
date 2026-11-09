// main/main.c —— 电子身份卡 · 宠物养成固件入口。
// 初始化 I2C/显示/背光/电池后交给 pet_app（卡片首页 + 召唤 + 成长），最后常开配网门户。
#include "bsp_battery.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "pet_app.h"
#include "weather.h"
#include "wifi_portal.h"

static const char *TAG = "main";

void app_main(void) {
    ESP_LOGI(TAG, "PET PASSPORT 启动");

    bsp_i2c_init();

    if (bsp_display_init() != ESP_OK) {
        ESP_LOGE(TAG, "显示初始化失败,检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    if (!bsp_lvgl_init()) {
        ESP_LOGE(TAG, "LVGL 初始化失败");
        return;
    }
    bsp_display_backlight(100);

    const esp_err_t battery = bsp_battery_init();
    if (battery != ESP_OK) {
        ESP_LOGW(TAG, "电池初始化失败,电量将显示为 --%%: %s", esp_err_to_name(battery));
    }

    pet_app_start();

    // 常开 SoftAP 门户：手机连 AI-PASSPORT-XXXX 后访问 192.168.4.1 配网/改昵称。
    // 同时负责 SNTP 校时——4 小时冷却的时间基准来自这里。
    wifi_portal_start();

    // 联网后后台取当天天气（wttr.in），失败时场景回退默认背景。
    weather_start();
}
