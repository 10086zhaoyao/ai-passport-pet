// main/screen_capture.c
// 串口取帧（仅观测）：收到 "FAP_SCREENSHOT_V1\n" 后，把当前 LVGL 帧缓冲按
// RGB565LE 逐带写出。
// 说明：控制台本身就走 USB Serial/JTAG，这里优先复用控制台已安装的驱动；
// 若驱动不可用才退回 LL FIFO 轮询（LL 直写在长时间连续发送时会卡住 EP）。
#include "screen_capture.h"

#include <stdio.h>
#include <string.h>

#include "bsp_display.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "lvgl.h"
#include "pet_app.h"

static const char *TAG = "capture";
static bool s_drv; // USB Serial/JTAG 驱动是否可用

#define CAP_W 240
#define CAP_H 320
#define CAP_BYTES (CAP_W * CAP_H * 2)

static volatile bool s_active;
static uint32_t s_bytes;

// 写串口：优先走驱动（由 ISR 负责搬 FIFO，阻塞式写到全部完成）；
// 驱动不可用时退回 LL 轮询，只在 FIFO 写满时让出 1 tick。
static void cap_write(const uint8_t *data, size_t len) {
    if (s_drv) {
        while (len > 0) {
            const int w = usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(100));
            if (w > 0) {
                data += w;
                len -= (size_t)w;
            } else {
                vTaskDelay(1);
            }
        }
        return;
    }
    while (len > 0) {
        const int w = usb_serial_jtag_ll_write_txfifo(data, len);
        if (w > 0) {
            data += w;
            len -= (size_t)w;
            usb_serial_jtag_ll_txfifo_flush();
        } else {
            vTaskDelay(1);
        }
    }
}

static void capture_flush_cb(lv_event_t *event) {
    if (!s_active) return;
    lv_display_t *disp = lv_event_get_target(event);
    const lv_area_t *area = lv_event_get_param(event);
    lv_draw_buf_t *buf = lv_display_get_buf_active(disp);
    if (!area || !buf || !buf->data) return;

    const int32_t w = lv_area_get_width(area);
    const int32_t h = lv_area_get_height(area);
    if (w <= 0 || h <= 0) return;
    const uint32_t stride = buf->header.stride;
    for (int32_t y = 0; y < h; y++) {
        cap_write(buf->data + (size_t)y * stride, (size_t)w * 2);
    }
    s_bytes += (uint32_t)w * h * 2;
}

static void take_capture(void) {
    if (!bsp_lvgl_lock(8000)) {
        ESP_LOGW(TAG, "取帧加锁超时");
        return;
    }
    ESP_LOGI(TAG, "取帧加锁 ok");
    char header[64];
    const int n = snprintf(header, sizeof(header), "FAP_SCREENSHOT_V1 %d %d RGB565LE %d\n",
                           CAP_W, CAP_H, CAP_BYTES);
    cap_write((const uint8_t *)header, (size_t)n);

    // 取帧期间静音日志，避免日志字节混入图像数据。
    esp_log_level_set("*", ESP_LOG_NONE);
    s_bytes = 0;
    s_active = true;
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    s_active = false;
    esp_log_level_set("*", ESP_LOG_INFO);
    bsp_lvgl_unlock();

    ESP_LOGI(TAG, "取帧完成: %u/%d 字节", (unsigned)s_bytes, CAP_BYTES);
}

// 取帧（尤其整屏 lv_refr_now）是深递归，要 16KB 栈。但这块栈如果常驻，门户
// （AP+HTTP）期间本来就见底的堆会被它白占 12KB。改成：常驻监听任务只留 4KB，
// 真收到取帧指令时才临时起一个 16KB 任务，取完立刻退出还内存。
static SemaphoreHandle_t s_shot_done;
static char s_shot_scene[32];
static bool s_shot_scene_set;

static void shot_worker(void *arg) {
    (void)arg;
    if (s_shot_scene_set) {
        // 联调：先切场景，再取帧，免得人工按键。
        pet_app_shot_scene(s_shot_scene);
        // 场景切换有 180ms 位移动效：等动效跑完再取帧，否则会抓到半屏/上一页。
        vTaskDelay(pdMS_TO_TICKS(400));
        s_shot_scene_set = false;
    }
    take_capture();
    if (s_shot_done) xSemaphoreGive(s_shot_done);
    vTaskDelete(NULL);
}

static void request_shot(bool with_scene, const char *scene) {
    if (with_scene) {
        strlcpy(s_shot_scene, scene, sizeof(s_shot_scene));
        s_shot_scene_set = true;
    }
    if (xTaskCreate(shot_worker, "shot", 16384, NULL, 6, NULL) != pdPASS) {
        ESP_LOGW(TAG, "取帧任务创建失败（内存不足）");
        s_shot_scene_set = false;
        return;
    }
    if (s_shot_done) xSemaphoreTake(s_shot_done, portMAX_DELAY);
}

static void capture_task(void *arg) {
    (void)arg;
    char line[32];
    size_t n = 0;
    for (;;) {
        uint8_t buf[64];
        const int r = s_drv ? usb_serial_jtag_read_bytes(buf, sizeof(buf), 0)
                            : usb_serial_jtag_ll_read_rxfifo(buf, sizeof(buf));
        if (r > 0) {
            for (int i = 0; i < r; i++) {
                const char c = (char)buf[i];
                if (c == '\n' || c == '\r') {
                    line[n] = '\0';
                    if (strcmp(line, "FAP_SCREENSHOT_V1") == 0) {
                        request_shot(false, NULL);
                    } else if (strncmp(line, "FAP_SHOT ", 9) == 0) {
                        request_shot(true, line + 9);
                    } else if (strncmp(line, "FAP_TEST ", 9) == 0) {
                        pet_app_test_cmd(line + 9);
                    }
                    n = 0;
                } else if (n < sizeof(line) - 1) {
                    line[n++] = c;
                }
            }
        }
        // 无条件让出：FIFO 可能持续报告可读，不能让本任务空转饿死 IDLE。
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void screen_capture_start(void) {
    lv_display_t *disp = lv_display_get_default();
    if (!disp) {
        ESP_LOGW(TAG, "LVGL display 未就绪，跳过取帧");
        return;
    }

    // 整屏 150KB，控制台的逐字节 VFS 写太慢，装驱动走环形缓冲 + ISR。
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 8192,
        .rx_buffer_size = 1024,
    };
    const esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        s_drv = true;
    }
    ESP_LOGI(TAG, "取帧通道: %s (install=%d)", s_drv ? "driver" : "ll", (int)err);

    lv_display_add_event_cb(disp, capture_flush_cb, LV_EVENT_FLUSH_START, NULL);
    if (!s_shot_done) s_shot_done = xSemaphoreCreateBinary();
    // 常驻监听只解析串口指令，栈压到 4KB；真正的 16KB 深栈只在取帧那一刻临时申请
    // （见 request_shot）。门户期间这 12KB 直接让给 WiFi/HTTP。
    // 优先级 6：宠物场景渲染占满 CPU（fps≈12、每帧 ~85ms）时，优先级 2 会一直抢
    // 不到 LVGL 锁；抬到 app_task(4) 之上保证取帧能插入。
    if (xTaskCreate(capture_task, "capture", 4096, NULL, 6, NULL) != pdPASS) {
        ESP_LOGW(TAG, "取帧任务创建失败");
    }
}
