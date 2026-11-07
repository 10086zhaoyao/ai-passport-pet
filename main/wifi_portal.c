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
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pet_app.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "profile_store.h"

static const char *TAG = "portal";

#define WIFI_NS "wifi"
#define PORTAL_IDLE_US (180LL * 1000000)

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
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_ssid[0]) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_ssid[0] && s_wifi_ready) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "STA 已连接: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        start_sntp();
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

// 手机端 Q 版头像页：中心裁剪 → 缩放到 64x64 → 色调分离 + 边缘强化 →
// 1-bit 化实时预览（可调阈值）→ 上传 512 字节。设备不做图像处理。
static const char *PAGE =
    "<!doctype html><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>AI Passport</title><style>"
    "body{margin:0;background:#0e1420;color:#e8f3ff;font-family:system-ui,sans-serif}"
    "main{max-width:420px;margin:0 auto;padding:18px}"
    "h1{font-size:19px;margin:4px 0 16px}"
    "label{display:block;margin:12px 0 4px;font-size:13px;color:#9fb4cc}"
    "input{width:100%;padding:10px;border-radius:8px;border:1px solid #2c4152;"
    "background:#16202f;color:#e8f3ff;box-sizing:border-box;font-size:15px}"
    "button{width:100%;margin-top:14px;padding:12px;border:0;border-radius:8px;"
    "background:#2aa5ff;color:#04121f;font-weight:700;font-size:15px}"
    "#cv{width:128px;height:128px;border:1px solid #2c4152;border-radius:8px;"
    "background:#fff;image-rendering:pixelated;margin:8px 0}"
    "#msg{font-size:13px;color:#7ee0b0;margin-top:10px;min-height:18px}"
    ".row{display:flex;gap:12px;align-items:center}"
    "</style><main><h1>AI PASSPORT</h1>"
    "<label>Nickname (max 10)</label><input id=nick maxlength=10>"
    "<label>Signature (max 20)</label><input id=sign maxlength=20>"
    "<label>Q-Avatar: pick a photo</label>"
    "<input id=f type=file accept='image/*'>"
    "<div class=row><canvas id=cv width=64 height=64></canvas>"
    "<div style=flex:1><label>Threshold <span id=tv>128</span></label>"
    "<input id=th type=range min=80 max=170 value=128 oninput=refilter()></div></div>"
    "<button onclick=up()>UPLOAD Q-AVATAR</button>"
    "<label>WiFi SSID (for time sync)</label><input id=ssid maxlength=32>"
    "<label>WiFi Password</label><input id=pass type=password maxlength=64>"
    "<button onclick=save()>SAVE &amp; CONNECT</button>"
    "<div id=msg></div></main><script>"
    "var C=document.getElementById('cv'),X=C.getContext('2d'),img=null,thr=128;"
    "X.fillStyle='#fff';X.fillRect(0,0,64,64);"
    "function m(t){document.getElementById('msg').textContent=t}"
    "document.getElementById('f').onchange=function(e){var f=e.target.files[0];"
    "if(!f)return;var i=new Image();i.onload=function(){img=i;refilter()};"
    "i.src=URL.createObjectURL(f)};"
    "function refilter(){thr=+document.getElementById('th').value;"
    "document.getElementById('tv').textContent=thr;if(!img){return}"
    "var s=Math.min(img.width,img.height);"
    "X.fillStyle='#fff';X.fillRect(0,0,64,64);"
    "X.drawImage(img,(img.width-s)/2,(img.height-s)/2,s,s,0,0,64,64);"
    "var d=X.getImageData(0,0,64,64),p=d.data,L=[],i,x,y,v;"
    "for(i=0;i<4096;i++)L[i]=(p[i*4]*299+p[i*4+1]*587+p[i*4+2]*114)/1000;"
    "var B=L.slice(0);for(y=1;y<63;y++)for(x=1;x<63;x++){i=y*64+x;"
    "B[i]=(L[i]*4+L[i-1]+L[i+1]+L[i-64]+L[i+64])/8}"
    "for(i=0;i<4096;i++){x=i&63;y=i>>6;var ink=0;"
    "if(B[i]<thr-22)ink=1;"
    "if(!ink&&x<63&&Math.abs(B[i]-B[i+1])>34)ink=1;"
    "if(!ink&&y<63&&Math.abs(B[i]-B[i+64])>34)ink=1;"
    "var c=ink?0x14:0xff,p2=ink?0x20:0xfd,p3=ink?0x2b:0xf2;"
    "p[i*4]=c;p[i*4+1]=p2;p[i*4+2]=p3;p[i*4+3]=255}"
    "X.putImageData(d,0,0)}"
    "function up(){var d=X.getImageData(0,0,64,64).data,o=new Uint8Array(512);"
    "for(var i=0;i<4096;i++){var r=d[i*4],g=d[i*4+1],b=d[i*4+2];"
    "if(r<0x80&&g<0x80&&b<0x80)o[i>>3]|=128>>(i&7)}"
    "fetch('/avatar',{method:'POST',body:o}).then(r=>r.text()).then(m)}"
    "function save(){var b=new URLSearchParams();"
    "b.set('ssid',document.getElementById('ssid').value);"
    "b.set('pass',document.getElementById('pass').value);"
    "b.set('nick',document.getElementById('nick').value);"
    "b.set('sign',document.getElementById('sign').value);"
    "fetch('/save',{method:'POST',body:b}).then(r=>r.text()).then(m)}"
    "</script>";

static void touch(httpd_req_t *req) {
    (void)req;
    s_last_req_us = esp_timer_get_time();
}

static esp_err_t h_root(httpd_req_t *req) {
    touch(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_save(httpd_req_t *req) {
    touch(req);
    char body[600];
    char ssid[33] = {0}, pass[65] = {0};
    char nick[PROFILE_NICK_LEN] = {0}, sign[PROFILE_SIGN_LEN] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return httpd_resp_sendstr(req, "Bad request");
    }
    form_get(body, "ssid", ssid, sizeof(ssid));
    form_get(body, "pass", pass, sizeof(pass));
    form_get(body, "nick", nick, sizeof(nick));
    form_get(body, "sign", sign, sizeof(sign));

    profile_t p;
    profile_load(&p);
    if (nick[0]) strlcpy(p.nick, nick, sizeof(p.nick));
    if (sign[0]) strlcpy(p.sign, sign, sizeof(p.sign));
    profile_save(&p);

    if (ssid[0]) {
        strlcpy(s_ssid, ssid, sizeof(s_ssid));
        strlcpy(s_pass, pass, sizeof(s_pass));
        save_creds(s_ssid, s_pass);
        wifi_config_t wc = {0};
        strlcpy((char *)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid));
        strlcpy((char *)wc.sta.password, s_pass, sizeof(wc.sta.password));
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        esp_wifi_disconnect();
        esp_wifi_connect();
    }

    pet_app_refresh();
    return httpd_resp_sendstr(req, "Saved. Connecting to WiFi...");
}

static esp_err_t h_avatar(httpd_req_t *req) {
    touch(req);
    uint8_t buf[PROFILE_AVATAR_BYTES];
    if (req->content_len != PROFILE_AVATAR_BYTES) {
        return httpd_resp_sendstr(req, "Need exactly 512 bytes");
    }
    int got = 0;
    while (got < PROFILE_AVATAR_BYTES) {
        const int r = httpd_req_recv(req, (char *)buf + got, PROFILE_AVATAR_BYTES - got);
        if (r <= 0) return httpd_resp_sendstr(req, "Recv failed");
        got += r;
    }
    if (!profile_save_avatar(buf)) {
        return httpd_resp_sendstr(req, "Save failed");
    }
    pet_app_refresh();
    return httpd_resp_sendstr(req, "Avatar updated");
}

static esp_err_t h_status(httpd_req_t *req) {
    touch(req);
    char json[96];
    snprintf(json, sizeof(json), "{\"sta\":%s}", s_connected ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static void start_http(void) {
    if (s_http) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_http, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败");
        return;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_root},
        {.uri = "/save", .method = HTTP_POST, .handler = h_save},
        {.uri = "/avatar", .method = HTTP_POST, .handler = h_avatar},
        {.uri = "/status", .method = HTTP_GET, .handler = h_status},
    };
    for (int i = 0; i < 4; i++) httpd_register_uri_handler(s_http, &uris[i]);
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
    esp_netif_init();
    esp_event_loop_create_default();  // 已存在时返回 INVALID_STATE，忽略
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_evt, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_evt, NULL, NULL);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "AI-PASSPORT-%02X%02X", mac[4], mac[5]);
    // 密码由设备 MAC 派生（每台不同），只显示在设备屏幕上，不写进任何公开文档。
    snprintf(s_ap_pass, sizeof(s_ap_pass), "folo-%02X%02X%02X", mac[3], mac[4], mac[5]);

    wifi_config_t sta = {0};
    if (s_ssid[0]) {
        strlcpy((char *)sta.sta.ssid, s_ssid, sizeof(sta.sta.ssid));
        strlcpy((char *)sta.sta.password, s_pass, sizeof(sta.sta.password));
    }

    // 开机仅 STA：有凭据就自动重连，联网成功后 SNTP 授时。热点按需再开。
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (s_ssid[0]) ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_ready = true;

    xTaskCreate(net_task, "net", 4096, NULL, 4, NULL);
}

bool wifi_portal_open(void) {
    if (s_portal_on) return true;
    if (!s_wifi_ready) return false;

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    strlcpy((char *)ap.ap.password, s_ap_pass, sizeof(ap.ap.password));
    ap.ap.channel = 6;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_stop();
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "热点启动失败");
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
        return false;
    }
    start_http();
    s_last_req_us = esp_timer_get_time();
    s_portal_on = true;
    ESP_LOGI(TAG, "门户开启: %s  http://192.168.4.1", s_ap_ssid);
    return true;
}

void wifi_portal_close(void) {
    if (!s_portal_on) return;
    stop_http();
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();  // STA_START 事件里会自动重连路由器
    s_portal_on = false;
    ESP_LOGI(TAG, "门户关闭");
}

bool wifi_portal_is_open(void) {
    return s_portal_on;
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
