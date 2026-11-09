// main/wifi_portal.c
// STA 常驻联网（NTP 授时）+ 按需开启的 SoftAP 自定义门户。
//
// 开机：仅 STA 模式，按已存凭据自动重连，联网成功后 SNTP 授时（4h 冷却的时间基准）。
// 门户：设置菜单进入「手机自定义」时开启 SoftAP（WPA2，密码由 MAC 派生，显示在屏幕）
// + HTTP 服务。手机连热点后访问 http://192.168.4.1 改昵称/签名/上传 Q 版头像。
// 照片到 Q 版的转换全部在手机浏览器 JS 里完成，设备只收 512 字节 1-bit 结果。
#include "wifi_portal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_rom_crc.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pet_app.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "profile_store.h"
#include "portal_html.h"

static const char *TAG = "portal";

#define WIFI_NS "wifi"
// 空闲多久自动关门。真人操作要「连热点 → 找到浏览器 → 输地址 → 传图」，
// 180s 太紧（实测手机刚连上就到了关门点，热点还在但 HTTP 已停）。
// 客户端接入/断开都会重置计时，所以这个值只兜“彻底没人管”的情况。
#define PORTAL_IDLE_US (600LL * 1000000)

static char s_ap_ssid[33];
static char s_ap_pass[65];
static char s_ssid[33];
static char s_pass[65];
static bool s_connected;
static bool s_time_synced;
static bool s_wifi_ready;
static bool s_portal_on;
static int64_t s_last_req_us;
static httpd_handle_t s_http;
// 最近一次开热点失败的原因，屏幕按这个字符串直接显示（LOW MEM / AP FAIL / HTTP FAIL）。
static char s_open_err[16];

// 开 AP 前要求的最小系统堆余量：C3 上切 APSTA 会一次性申请数百字节的 WiFi
// 动态缓冲并创建 AP 任务/队列，低于这个值必然 alloc 失败并复位。
#define PORTAL_MIN_FREE_BYTES (40 * 1024)
#define PORTAL_MIN_LARGEST_BYTES (12 * 1024)

static void ensure_ap_identity(void);
static esp_err_t wifi_lazy_init(void);
static void ensure_netifs(void);

// ---------------------------------------------------------------------------
// NVS 凭据
// ---------------------------------------------------------------------------
static void load_creds(void) {
    nvs_handle_t h;
    if (nvs_open(WIFI_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_ssid);
    (void)nvs_get_str(h, "ssid", s_ssid, &len);
    len = sizeof(s_pass);
    (void)nvs_get_str(h, "pass", s_pass, &len);
    nvs_close(h);
}

static void save_creds(const char *ssid, const char *pass) {
    nvs_handle_t h;
    if (nvs_open(WIFI_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    (void)nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------------------
// WiFi 事件与 NTP
// ---------------------------------------------------------------------------
static void start_sntp(void) {
    if (esp_sntp_enabled()) return;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "ntp.aliyun.com");
    esp_sntp_init();
}

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_ssid[0] && !s_portal_on) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_portal_on) {
            // 门户期间不做 STA 重连：避免信道切换把已接入的手机踢下线。
            ESP_LOGI(TAG, "STA 断开（门户开启，暂停重连）");
        } else if (s_ssid[0] && s_wifi_ready) {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "STA 已连接: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        start_sntp();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "AP_START 热点已启动");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *ev = (const wifi_event_ap_staconnected_t *)data;
        // 接入就重置空闲计时：手机上还要开浏览器、输地址，不该算“没人用”。
        s_last_req_us = esp_timer_get_time();
        ESP_LOGI(TAG, "AP 客户端接入 " MACSTR " aid=%d free=%uB largest=%uB", MAC2STR(ev->mac),
                 ev->aid, (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *ev =
            (const wifi_event_ap_stadisconnected_t *)data;
        s_last_req_us = esp_timer_get_time();
        ESP_LOGW(TAG, "AP 客户端断开 " MACSTR " aid=%d reason=%d", MAC2STR(ev->mac), ev->aid,
                 ev->reason);
    }
}

// ---------------------------------------------------------------------------
// HTTP 表单
// ---------------------------------------------------------------------------
static void url_decode(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') {
            *o++ = ' ';
        } else if (*p == '%' && isxdigit((unsigned char)p[1]) &&
                   isxdigit((unsigned char)p[2])) {
            char hex[3] = {p[1], p[2], 0};
            *o++ = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            *o++ = *p;
        }
    }
    *o = 0;
}

static bool form_get(const char *body, const char *key, char *out, size_t n) {
    char tmp[640];
    strlcpy(tmp, body, sizeof(tmp));
    char *save = NULL;
    for (char *kv = strtok_r(tmp, "&", &save); kv; kv = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(kv, '=');
        if (!eq) continue;
        *eq = 0;
        if (strcmp(kv, key) != 0) continue;
        url_decode(eq + 1);
        strlcpy(out, eq + 1, n);
        return true;
    }
    return false;
}

static int read_body(httpd_req_t *req, char *buf, size_t cap) {
    if (req->content_len == 0 || req->content_len >= (int)cap) return -1;
    int got = 0;
    while (got < (int)req->content_len) {
        const int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) return -1;
        got += r;
    }
    buf[got] = 0;
    return got;
}

// 配网页终稿已改为 portal_html.c 里的 gzip 字节数组（见 h_root）。

static void touch(httpd_req_t *req) {
    (void)req;
    s_last_req_us = esp_timer_get_time();
}

// 按 UTF-8 字符数截断（不是字节），避免把汉字截成半个。
static void utf8_trim(char *s, int max_chars) {
    int n = 0;
    char *p = s;
    while (*p) {
        const unsigned char c = (unsigned char)*p;
        const int len = (c < 0x80) ? 1 : ((c >> 5) == 0x6 ? 2 : ((c >> 4) == 0xE ? 3 : 4));
        if (n >= max_chars) {
            *p = '\0';
            return;
        }
        p += len;
        n++;
    }
}

// 按字节数截断，但只在 UTF-8 字符边界上切，避免留下半个汉字。
static void utf8_trim_bytes(char *s, size_t max_bytes) {
    size_t n = strlen(s);
    while (n > max_bytes) {
        n--;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
        s[n] = '\0';
    }
}

// JSON 字符串转义：只处理 " 与 \，其余控制字符降为空格；不产生 JSON 非法转义。
static void json_esc(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    for (const char *p = src; *p && o + 2 < cap; p++) {
        const unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c < 0x20) {
            dst[o++] = ' ';
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

// 配网页终稿：gzip 字节数组 + 显式长度，绝不经过格式化或字符串函数。
// 客户端没带 Accept-Encoding: gzip 时回原文，避免白屏（多花约 61KB flash）。
static esp_err_t h_root(httpd_req_t *req) {
    touch(req);
    char enc[48] = {0};
    const bool gz = httpd_req_get_hdr_value_str(req, "Accept-Encoding", enc, sizeof(enc)) == ESP_OK &&
                    strstr(enc, "gzip") != NULL;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    // 证据日志：页面上线/发完各一条，带上堆水位，便于判断“200 但发不完”还是真 200。
    const uint32_t h0 = esp_get_free_heap_size();
    ESP_LOGI(TAG, "GET / 开始: %s %d B, free=%uB largest=%uB",
             gz ? "gzip" : "raw", (int)(gz ? portal_html_gz_len : portal_html_raw_len),
             (unsigned)h0,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    const esp_err_t e = gz ? httpd_resp_send(req, (const char *)portal_html_gz,
                                             (ssize_t)portal_html_gz_len)
                           : httpd_resp_send(req, (const char *)portal_html_raw,
                                             (ssize_t)portal_html_raw_len);
    ESP_LOGI(TAG, "GET / 完成: %s, free=%uB largest=%uB", esp_err_to_name(e),
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    return e;
}

static esp_err_t h_save(httpd_req_t *req) {
    touch(req);
    char body[1200];
    char ssid[33] = {0}, pass[65] = {0};
    char nick[PROFILE_NICK_LEN] = {0}, sign[PROFILE_SIGN_LEN] = {0};
    char petname[PROFILE_PETNAME_LEN] = {0};
    char sex[8] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return httpd_resp_sendstr(req, "too long");
    }
    // 终稿字段名：nickname / signature / gender / ssid / password / style。
    // 旧名（nick / sign / sex / pass）继续兼容，避免旧页面或手工请求失效。
    if (!form_get(body, "nickname", nick, sizeof(nick))) form_get(body, "nick", nick, sizeof(nick));
    if (!form_get(body, "signature", sign, sizeof(sign))) form_get(body, "sign", sign, sizeof(sign));
    if (!form_get(body, "gender", sex, sizeof(sex))) form_get(body, "sex", sex, sizeof(sex));
    const bool has_pass = form_get(body, "password", pass, sizeof(pass)) ||
                          form_get(body, "pass", pass, sizeof(pass));
    form_get(body, "ssid", ssid, sizeof(ssid));
    // 长度上限在设备端再兜一层：昵称 12 字、签名 24 字。
    utf8_trim(nick, 12);
    utf8_trim(sign, 24);
    // 宠物名：只要页面传了这个字段就采纳（空串也采纳 → 渲染时回落默认名）。
    // 上限 5 个汉字，与配网页一致；落款行 20 + 40 = 60 px，最坏天气下仍有 4 px 余量。
    const bool has_petname = form_get(body, "petname", petname, sizeof(petname));
    utf8_trim(petname, 5);
    utf8_trim_bytes(petname, PROFILE_PETNAME_LEN - 1);

    profile_t p;
    profile_load(&p);
    if (nick[0]) strlcpy(p.nick, nick, sizeof(p.nick));
    if (has_petname) strlcpy(p.petname, petname, sizeof(p.petname));
    if (sign[0]) strlcpy(p.sign, sign, sizeof(p.sign));
    // 0=男生（默认），1=女生。终稿页面传 male/female，旧页面传 0/1。
    if (sex[0]) {
        p.sex = (sex[0] == '1' || sex[0] == 'f' || sex[0] == 'F') ? 1 : 0;
    }
    profile_save(&p);

    if (ssid[0]) {
        char final_pass[65] = {0};
        if (has_pass) {
            strlcpy(final_pass, pass, sizeof(final_pass));
        } else if (strcmp(ssid, s_ssid) == 0) {
            strlcpy(final_pass, s_pass, sizeof(final_pass));  // 没带密码且 SSID 未变：沿用旧密码
        }
        strlcpy(s_ssid, ssid, sizeof(s_ssid));
        strlcpy(s_pass, final_pass, sizeof(s_pass));
        save_creds(s_ssid, s_pass);
        wifi_config_t wc = {0};
        strlcpy((char *)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid));
        strlcpy((char *)wc.sta.password, s_pass, sizeof(wc.sta.password));
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        esp_wifi_disconnect();
        s_connected = false;  // 断开后立刻置假，避免 /status 返回上一次连接的旧真值
        esp_wifi_connect();
    }

    pet_app_refresh();
    return httpd_resp_sendstr(req, "OK");
}

// ---------------------------------------------------------------------------
// 头像上传：只收 88x88 真彩 23,232 B（15,488 B RGB565 小端 + 7,744 B A8），
// Content-Length 不符直接 400，不静默截断。按 3,872 B 一段收、收一段落一段 NVS，
// 收满 6 段后写 meta（提交标志）并回 202。页面轮询 GET /avatar/status。
// ---------------------------------------------------------------------------
enum { AV_IDLE = 0, AV_SAVING, AV_SAVED, AV_FAILED };
static volatile int s_av_state = AV_IDLE;
static char s_av_reason[12] = "";
// 收包缓冲：正好一段 3,872 B。上传期间临时申请、结束立刻释放 —— 不能做成静态，
// 实测静态占 3,872 B 会把 AP 起来后留给 httpd_start 的空间挤掉（门户 HTTP 启动失败）。
// 峰值仍然只有一段，从不出现 23,232 B 连续分配。
static uint8_t *s_av_seg;

static esp_err_t av_json(httpd_req_t *req, const char *status, const char *body) {
    if (status) httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t h_avatar_status(httpd_req_t *req) {
    touch(req);
    static const char *ST[] = {"idle", "saving", "saved", "failed"};
    char json[64];
    snprintf(json, sizeof(json), "{\"state\":\"%s\",\"reason\":\"%s\"}",
             ST[s_av_state], s_av_reason);
    // 失败态用 507/500 回，页面不必再判 reason 才知道失败。
    const char *code = NULL;
    if (s_av_state == AV_FAILED) {
        code = (strcmp(s_av_reason, "nvs_full") == 0) ? "507 Insufficient Storage"
                                                      : "500 Internal Server Error";
    }
    return av_json(req, code, json);
}

static esp_err_t h_avatar(httpd_req_t *req) {
    touch(req);
    // multipart（PNG）没有解码器，明确拒绝，不能假装成功。
    char ct[48] = "";
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct)) == ESP_OK &&
        strstr(ct, "multipart") != NULL) {
        return av_json(req, "415 Unsupported Media Type",
                       "{\"state\":\"failed\",\"reason\":\"format\"}");
    }
    // 只认 88x88 真彩（23,232 B）。长度不符直接拒，并把期望值和实收值回给页面，
    // 绝不静默截断或按旧格式猜。
    if (req->content_len != PROFILE_AVATAR_BYTES) {
        char msg[112];
        snprintf(msg, sizeof(msg),
                 "{\"state\":\"failed\",\"reason\":\"size\",\"expect\":%d,\"got\":%d}",
                 PROFILE_AVATAR_BYTES, (int)req->content_len);
        return av_json(req, "400 Bad Request", msg);
    }

    // 23,232 B 按 6 段收：每段收满立刻写 NVS，内存里永远只有一段（3,872 B）。
    // A/B：写进“当前没生效”的那个 bank，meta 最后写并切过去；中途断开 meta 还指着旧 bank，
    // 重启后仍是旧头像（不是花屏、也不是空框）。
    if (s_av_state == AV_SAVING) {
        return av_json(req, "409 Conflict", "{\"state\":\"busy\"}");
    }
    s_av_seg = malloc(PROFILE_AVATAR_SEG_BYTES);
    if (!s_av_seg) {
        return av_json(req, "503 Service Unavailable",
                       "{\"state\":\"failed\",\"reason\":\"nomem\"}");
    }
    const uint8_t cur_bank = profile_avatar_bank_in_use();
    const uint8_t bank = (cur_bank == 0) ? 1 : 0;
    s_av_reason[0] = 0;
    s_av_state = AV_SAVING;
    uint32_t crc = 0;
    esp_err_t fail = ESP_OK;
    for (uint8_t i = 0; i < PROFILE_AVATAR_SEGS && fail == ESP_OK; i++) {
        size_t got = 0;
        while (got < PROFILE_AVATAR_SEG_BYTES) {
            const int r = httpd_req_recv(req, (char *)s_av_seg + got,
                                         PROFILE_AVATAR_SEG_BYTES - got);
            if (r <= 0) {
                // 半张图：meta 没写，下次启动仍回旧 bank 的旧形象。
                ESP_LOGW(TAG, "头像收包中断: 第 %u 段 +%u B", (unsigned)i, (unsigned)got);
                snprintf(s_av_reason, sizeof(s_av_reason), "recv");
                s_av_state = AV_FAILED;
                fail = ESP_FAIL;
                break;
            }
            got += (size_t)r;
        }
        if (fail != ESP_OK) break;
        crc = esp_rom_crc32_le(crc, s_av_seg, PROFILE_AVATAR_SEG_BYTES);
        fail = (esp_err_t)profile_avatar_write_seg(bank, i, s_av_seg,
                                                   PROFILE_AVATAR_SEG_BYTES);
        if (fail != ESP_OK) {
            snprintf(s_av_reason, sizeof(s_av_reason), "%s",
                     fail == ESP_ERR_NVS_NOT_ENOUGH_SPACE ? "nvs_full" : "io");
            s_av_state = AV_FAILED;
            ESP_LOGW(TAG, "头像第 %u 段落盘失败: %s", (unsigned)i, esp_err_to_name(fail));
        }
    }
    free(s_av_seg);
    s_av_seg = NULL;
    if (fail == ESP_FAIL) {
        return av_json(req, "500 Internal Server Error",
                       "{\"state\":\"failed\",\"reason\":\"recv\"}");
    }
    if (fail != ESP_OK) {
        return av_json(req,
                       fail == ESP_ERR_NVS_NOT_ENOUGH_SPACE ? "507 Insufficient Storage"
                                                           : "500 Internal Server Error",
                       "{\"state\":\"failed\",\"reason\":\"nvs\"}");
    }
    // 6 段已落盘，补写 meta 作为提交标志（只有 16 B，同步写完即可，不必再起任务）。
    const esp_err_t e_commit = (esp_err_t)profile_avatar_commit(bank, PROFILE_AVATAR_BYTES, crc);
    if (e_commit != ESP_OK) {
        snprintf(s_av_reason, sizeof(s_av_reason), "%s",
                 e_commit == ESP_ERR_NVS_NOT_ENOUGH_SPACE ? "nvs_full" : "io");
        s_av_state = AV_FAILED;
        ESP_LOGW(TAG, "头像提交失败: %s", esp_err_to_name(e_commit));
        return av_json(req,
                       e_commit == ESP_ERR_NVS_NOT_ENOUGH_SPACE ? "507 Insufficient Storage"
                                                               : "500 Internal Server Error",
                       "{\"state\":\"failed\",\"reason\":\"nvs\"}");
    }
    s_av_reason[0] = 0;
    s_av_state = AV_SAVED;
    // 门户期间 avatar_build() 会跳过重建，退出后回首页时再从 flash 回源。
    pet_app_refresh();
    httpd_resp_set_hdr(req, "Retry-After", "1");
    char msg[72];
    snprintf(msg, sizeof(msg),
             "{\"state\":\"saving\",\"retry_ms\":500,\"bytes\":%d}", PROFILE_AVATAR_BYTES);
    return av_json(req, "202 Accepted", msg);
}

// 扫描周边 AP：终稿页面第 2 步选网用。APSTA 下扫描会让热点短暂停摆 1~3s，
// 手机端 16s 超时 + 失败重试，能兜住。JSON 直接给 [{"ssid","rssi","auth"}]。
#define SCAN_MAX_APS 12
static esp_err_t h_scan(httpd_req_t *req) {
    touch(req);
    static wifi_ap_record_t recs[SCAN_MAX_APS];
    static char json[1400];
    wifi_scan_config_t sc = {0};
    sc.show_hidden = false;
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "[]");
    }
    uint16_t n = SCAN_MAX_APS;
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) n = 0;
    size_t o = 0;
    int cnt = 0;
    json[o++] = '[';
    for (uint16_t i = 0; i < n && o + 96 < sizeof(json); i++) {
        if (!recs[i].ssid[0]) continue;
        char ssid[33 * 2];
        json_esc(ssid, sizeof(ssid), (const char *)recs[i].ssid);
        o += (size_t)snprintf(json + o, sizeof(json) - o,
                              "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                              cnt ? "," : "", ssid, (int)recs[i].rssi, (int)recs[i].authmode);
        cnt++;
    }
    json[o++] = ']';
    json[o] = '\0';
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

// 终稿页面加载时读：昵称/签名/宠物名/性别/上一次 SSID，用于回填与状态灯。
static esp_err_t h_status(httpd_req_t *req) {
    touch(req);
    profile_t p;
    profile_load(&p);
    char nick[PROFILE_NICK_LEN * 2];
    char sign[PROFILE_SIGN_LEN * 2];
    char petname[PROFILE_PETNAME_LEN * 2];
    char ssid[sizeof(s_ssid) * 2];
    json_esc(nick, sizeof(nick), p.nick);
    json_esc(sign, sizeof(sign), p.sign);
    json_esc(petname, sizeof(petname), p.petname);
    json_esc(ssid, sizeof(ssid), s_ssid);
    char json[512];
    snprintf(json, sizeof(json),
             "{\"sta\":%s,\"nickname\":\"%s\",\"signature\":\"%s\",\"petname\":\"%s\","
             "\"gender\":\"%s\",\"ssid\":\"%s\",\"has_password\":%s,\"lang\":\"%s\"}",
             s_connected ? "true" : "false", nick, sign, petname, p.sex ? "female" : "male", ssid,
             s_pass[0] ? "true" : "false", p.lang ? "en" : "zh");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static bool start_http(void) {
    if (s_http) return true;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;
    // C3 没有 PSRAM。IDF 固定预留 3 个 socket 给 HTTP 服务内部，默认 7 只余 4 个给客户端；
    // 压到 5（余 2 个客户端位）既够一台手机刷新页面，又省下两条 socket 的内存，
    // 降低 AP 起来后 httpd_start 申请不到内存的概率。
    cfg.max_open_sockets = 4;
    cfg.backlog_conn = 3;
    // 默认 4KB 栈发整页（约 4.5KB 的 HTML，两次 chunk）会踩栈复位——实测 GET /
    // 直接 rst:0xc。AP 起来后实测 free=17.6KB / largest=9.7KB，8KB 栈一次就把
    // 最大块吃掉，后续 socket 申请失败（HTTP 服务启动失败）；收到 6KB 兼顾两者。
    cfg.stack_size = 6144;
    esp_err_t err = httpd_start(&s_http, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败 err=%s", esp_err_to_name(err));
        s_http = NULL;
        return false;
    }
    ESP_LOGI(TAG, "HTTP 已就绪: port=%d stack=%d sockets=%d free=%uB largest=%uB",
             (int)cfg.server_port, (int)cfg.stack_size, (int)cfg.max_open_sockets,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_root},
        {.uri = "/save", .method = HTTP_POST, .handler = h_save},
        {.uri = "/avatar", .method = HTTP_POST, .handler = h_avatar},
        {.uri = "/avatar/status", .method = HTTP_GET, .handler = h_avatar_status},
        {.uri = "/status", .method = HTTP_GET, .handler = h_status},
        {.uri = "/scan", .method = HTTP_GET, .handler = h_scan},
    };
    for (int i = 0; i < 6; i++) httpd_register_uri_handler(s_http, &uris[i]);
    return true;
}

static void stop_http(void) {
    if (!s_http) return;
    httpd_stop(s_http);
    s_http = NULL;
}

// ---------------------------------------------------------------------------
// 后台：NTP 结果检查
// ---------------------------------------------------------------------------
static void net_task(void *arg) {
    (void)arg;
    for (;;) {
        if (s_connected && !s_time_synced &&
            sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            s_time_synced = true;
            const time_t now = time(NULL);
            pet_app_set_epoch((int64_t)now);
            ESP_LOGI(TAG, "NTP 授时完成");
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// ---------------------------------------------------------------------------
void wifi_portal_start(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    load_creds();
    ensure_netifs();
    // 关键：开机**不再** esp_wifi_init。C3 上这一步要占约 16KB 连续堆，
    // 开机就吃掉它，等用户点开配网再切 APSTA 时必然 alloc 失败。改为用户点开
    // 配网时才初始化（wifi_lazy_init），平时整块内存留给 LVGL/音频。
    ensure_ap_identity();

    xTaskCreate(net_task, "net", 4096, NULL, 4, NULL);
}

// STA/AP 的 esp_netif 只建一次。开热点也可能发生在开机流程跑到这里之前
// （串口钩子/按键抢跑），所以 wifi_portal_open() 也必须能自己兜底建 netif，
// 否则 AP 虽然广播、但没有 DHCP 服务器，手机连上拿不到 IP。
static void ensure_netifs(void) {
    esp_netif_init();
    esp_event_loop_create_default();  // 已存在时返回 INVALID_STATE，忽略
    if (!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
        esp_netif_create_default_wifi_sta();
    }
    // 门户热点必须有自己的 AP netif：192.168.4.1 地址、DHCP 服务器、收包回调都由它提供。
    // 只建 STA netif 时 WIFI_EVENT_AP_START 仍会触发、SSID 也会广播，但 esp_netif 的
    // AP 默认事件处理器没有注册（s_wifi_netifs[WIFI_IF_AP] 为 NULL），客户端连上后
    // 既拿不到 IP 也收不到任何数据，手机端表现为“能搜到但连不上”。
    if (!esp_netif_get_handle_from_ifkey("WIFI_AP_DEF")) {
        if (!esp_netif_create_default_wifi_ap()) {
            ESP_LOGE(TAG, "AP netif 创建失败，门户将不可用");
        }
    }
}

// SSID/密码由 MAC 派生。开机流程和"点开配网"可能并发，必须谁先用到谁现算，
// 否则会出现 set_config=ESP_ERR_INVALID_ARG（ssid 还是空串、热点不广播）的竞态。
static void ensure_ap_identity(void) {
    if (s_ap_ssid[0]) return;
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "AI-PASSPORT-%02X%02X", mac[4], mac[5]);
    // 密码由设备 MAC 派生（每台不同），只显示在设备屏幕上，不写进任何公开文档。
    snprintf(s_ap_pass, sizeof(s_ap_pass), "folo-%02X%02X%02X", mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "门户热点参数: ssid=%s pass_len=%u auth=WPA2-PSK ch=6（WiFi 延迟初始化）",
             s_ap_ssid, (unsigned)strlen(s_ap_pass));
}

// 首次需要联网时才初始化 WiFi 驱动。返回错误码，由调用方翻译成屏幕提示：
// ESP_ERR_NO_MEM → LOW MEM；其他 → WIFI FAIL。
static esp_err_t wifi_lazy_init(void) {
    if (s_wifi_ready) return ESP_OK;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init);
    ESP_LOGI(TAG, "wifi_lazy_init: esp_wifi_init=%s", esp_err_to_name(err));
    if (err != ESP_OK) return err;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_evt, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_evt, NULL, NULL);
    s_wifi_ready = true;
    return ESP_OK;
}

bool wifi_portal_open(void) {
    s_open_err[0] = 0;
    if (s_portal_on) return true;

    // 第一道闸：初始化 WiFi / 切 APSTA 之前先量堆。失败就报原因，不静默、不 abort。
    const size_t heap_free = esp_get_free_heap_size();
    const size_t heap_large =
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "门户: 开 AP 前堆 free=%uB largest=%uB（门槛 %uB/%uB）",
             (unsigned)heap_free, (unsigned)heap_large,
             (unsigned)PORTAL_MIN_FREE_BYTES, (unsigned)PORTAL_MIN_LARGEST_BYTES);
    if (heap_free < PORTAL_MIN_FREE_BYTES || heap_large < PORTAL_MIN_LARGEST_BYTES) {
        ESP_LOGE(TAG, "门户: 内存不足，不开热点（free=%uB largest=%uB）",
                 (unsigned)heap_free, (unsigned)heap_large);
        strlcpy(s_open_err, "LOW MEM", sizeof(s_open_err));
        return false;
    }

    // 第二道闸：此刻才初始化 WiFi 驱动（开机不占那 16KB）。失败原因上屏。
    ensure_netifs();
    esp_err_t e_init = wifi_lazy_init();
    if (e_init != ESP_OK) {
        strlcpy(s_open_err, e_init == ESP_ERR_NO_MEM ? "LOW MEM" : "WIFI FAIL",
                sizeof(s_open_err));
        return false;
    }

    // 兜底：若开机流程的 wifi_portal_start() 还没跑到（例如串口钩子在启动早期
    // 就触发了开热点），这里现算 SSID/密码，否则 set_config 会因空 ssid 报
    // ESP_ERR_INVALID_ARG(12299)，热点不广播。
    ensure_ap_identity();

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    strlcpy((char *)ap.ap.password, s_ap_pass, sizeof(ap.ap.password));
    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
    // WPA2-PSK 下彻底关闭 PMF：beacon 里不带 MFPC 位，避免部分手机/系统版本
    // 因 RSN 能力位不匹配而在关联阶段直接被拒绝。
    ap.ap.pmf_cfg.required = false;
    ap.ap.pmf_cfg.capable = false;
    ap.ap.beacon_interval = 100;

    // 不停 STA、不 stop/start 整个驱动：直接在已启动的 STA 上切 APSTA 再配 AP。
    // 之前先 disconnect + stop 再 start，在 C3 上会卡死在 stop 里（串口彻底静默），
    // 配网页永远打不开。这里只做模式切换，最坏情况是返回错误码走失败提示。
    s_portal_on = true;
    esp_err_t e_stop = ESP_OK;
    ESP_LOGI(TAG, "门户: 切 APSTA ...");
    esp_err_t e_mode = esp_wifi_set_mode(WIFI_MODE_APSTA);
    ESP_LOGI(TAG, "门户: set_mode=%d，写 AP 配置 ...", e_mode);
    esp_err_t e_cfg = esp_wifi_set_config(WIFI_IF_AP, &ap);
    // 有历史凭据就顺带配好 STA，AP 起来后自动重连家用路由（不阻塞热点）。
    if (s_ssid[0]) {
        wifi_config_t sta = {0};
        strlcpy((char *)sta.sta.ssid, s_ssid, sizeof(sta.sta.ssid));
        strlcpy((char *)sta.sta.password, s_pass, sizeof(sta.sta.password));
        esp_wifi_set_config(WIFI_IF_STA, &sta);
    }
    ESP_LOGI(TAG, "门户: set_config=%d，start ...", e_cfg);
    esp_err_t e_start = esp_wifi_start();
    ESP_LOGI(TAG, "门户启动: stop=%d mode=%d cfg=%d start=%d ssid=%s len=%u pass_len=%u ch=%d",
             e_stop, e_mode, e_cfg, e_start, s_ap_ssid, (unsigned)strlen(s_ap_ssid),
             (unsigned)strlen(s_ap_pass), ap.ap.channel);
    ESP_LOGI(TAG, "门户: 启动后堆 free=%uB largest=%uB",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    if (e_mode != ESP_OK || e_cfg != ESP_OK || e_start != ESP_OK) {
        ESP_LOGE(TAG, "热点启动失败");
        s_portal_on = false;
        strlcpy(s_open_err, (e_start != ESP_OK && esp_get_free_heap_size() < PORTAL_MIN_FREE_BYTES)
                                ? "LOW MEM" : "AP FAIL", sizeof(s_open_err));
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
        return false;
    }
    if (s_ssid[0]) esp_wifi_connect();
    if (!start_http()) {
        // 热点起来了但页面打不开，对用户没有意义，按失败处理并恢复 STA。
        ESP_LOGE(TAG, "门户 HTTP 启动失败");
        s_portal_on = false;
        strlcpy(s_open_err, "HTTP FAIL", sizeof(s_open_err));
        esp_wifi_stop();
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
        return false;
    }

    uint8_t ch = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&ch, &second);
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip = {0};
    if (ap_netif) esp_netif_get_ip_info(ap_netif, &ip);
    ESP_LOGI(TAG, "门户开启: ssid=%s 实际信道=%u ip=" IPSTR " http://192.168.4.1",
             s_ap_ssid, ch, IP2STR(&ip.ip));

    s_last_req_us = esp_timer_get_time();
    return true;
}

void wifi_portal_close(void) {
    if (!s_portal_on) return;
    stop_http();
    s_portal_on = false;  // 先清标志，恢复 STA 自动重连
    // 必须真关：只切模式不检查返回值时，C3 上可能出现“热点还在广播、HTTP 已停”，
    // 手机能连但打不开页面。这里校验 set_mode 结果并回读实际模式，最多重试 3 次。
    for (int i = 0; i < 3; i++) {
        const esp_err_t e = esp_wifi_set_mode(WIFI_MODE_STA);
        if (e == ESP_OK) {
            wifi_mode_t m = WIFI_MODE_NULL;
            if (esp_wifi_get_mode(&m) == ESP_OK && m == WIFI_MODE_STA) {
                ESP_LOGI(TAG, "门户关闭: 已切回 STA，热点已停");
                return;
            }
            ESP_LOGW(TAG, "门户关闭: 模式回读=%d，重试 %d", (int)m, i + 1);
        } else {
            ESP_LOGE(TAG, "门户关闭: set_mode 失败 %s，重试 %d", esp_err_to_name(e), i + 1);
        }
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    // 重试仍失败：至少让用户知道，不要静默留一个连得上但没服务的空壳热点。
    ESP_LOGE(TAG, "门户关闭: AP 可能仍在广播（模式切换失败）");
}

bool wifi_portal_is_open(void) {
    return s_portal_on;
}

long long wifi_portal_idle_us(void) {
    return (long long)PORTAL_IDLE_US;
}

bool wifi_portal_expired(void) {
    return s_portal_on && esp_timer_get_time() - s_last_req_us > PORTAL_IDLE_US;
}

long long wifi_portal_last_req_us(void) {
    return (long long)s_last_req_us;
}

const char *wifi_portal_ssid(void) {
    return s_ap_ssid;
}

const char *wifi_portal_pass(void) {
    return s_ap_pass;
}

bool wifi_portal_connected(void) {
    return s_connected;
}

const char *wifi_portal_last_error(void) {
    return s_open_err[0] ? s_open_err : "FAIL";
}
