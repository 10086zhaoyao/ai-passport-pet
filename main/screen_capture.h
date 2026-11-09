// main/screen_capture.h
// 串口取帧：实现发布助手的 FAP_SCREENSHOT_V1 协议（只读，不重启/不擦写）。
#pragma once

// 在 LVGL 就绪后调用一次：注册取帧回调并启动串口监听任务。
void screen_capture_start(void);
