#include "web_ui_api.h"
#include "app_config.h"

#include "sensor_manager.h"
#include "control_engine.h"
#include "heating_output.h"
#include "fault_manager.h"
#include "simulation_manager.h"
#include "storage_manager.h"
#include "network_manager.h"
#include "notification_manager.h"
#include "profile.h"
#include "lora_receiver.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "mbedtls/constant_time.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"

static const char *TAG = "web";

/* Embedded web assets (see main/CMakeLists.txt EMBED_FILES). */
extern const unsigned char index_html_start[] asm("_binary_index_html_gz_start");
extern const unsigned char index_html_end[]   asm("_binary_index_html_gz_end");
extern const unsigned char style_css_start[]  asm("_binary_style_css_gz_start");
extern const unsigned char style_css_end[]    asm("_binary_style_css_gz_end");
extern const unsigned char app_js_start[]     asm("_binary_app_js_gz_start");
extern const unsigned char app_js_end[]       asm("_binary_app_js_gz_end");

static system_config_t *s_cfg = NULL;
static httpd_handle_t   s_srv = NULL;

/* ---- static assets (gzip-embedded; browser decompresses) ---- */

/* History buffers for daily aggregates and logs (guarded by s_hist_mutex).
 * Minute samples are streamed directly from the storage ring buffer — no
 * need for a 1440-sample copy buffer here (saves ~57 KB DRAM BSS). */
static SemaphoreHandle_t s_hist_mutex = NULL;
static minute_sample_t s_daily[400];      /* up to ~12 months of daily aggregates */
static log_entry_t     s_logbuf[100];

/* ---- minimal JSON value extractors (we control the client) ---- */
static const char *find_key(const char *j, const char *key)
{
    char pat[48]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(j, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    return p;
}
static bool json_str(const char *j, const char *key, char *out, size_t len)
{
    const char *p = find_key(j, key);
    if (!p || *p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && i + 1 < len) {
        if (*p == '"') break;                  /* closing quote              */
        if (*p == '\\' && p[1]) {              /* keep escaped char literally */
            out[i++] = p[1];
            p += 2;
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = '\0';
    return true;
}
static bool json_float(const char *j, const char *key, float *out)
{
    const char *p = find_key(j, key);
    if (!p) return false;
    char *ep; float v = strtof(p, &ep);
    if (ep == p) return false;
    *out = v; return true;
}
static bool json_int(const char *j, const char *key, int *out)
{
    const char *p = find_key(j, key);
    if (!p) return false;
    char *ep; long v = strtol(p, &ep, 10);
    if (ep == p) return false;
    *out = (int)v; return true;
}
static bool json_bool(const char *j, const char *key, bool *out)
{
    const char *p = find_key(j, key);
    if (!p) return false;
    if (strncmp(p, "true", 4) == 0) { *out = true; return true; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
    return false;
}

/* ---- helpers ---- */
static esp_err_t send_json(httpd_req_t *req, const char *body)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}
static esp_err_t send_text(httpd_req_t *req, const char *body, int status)
{
    if (status != 200) httpd_resp_set_status(req, status == 400 ? "400 Bad Request" :
                                                        status == 500 ? "500 Internal Error" : "200 OK");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}
static int read_body(httpd_req_t *req, char *buf, size_t len)
{
    size_t want = req->content_len;
    if (want >= len) want = len - 1;
    size_t got = 0;
    int retries = 0;
    /* httpd_req_recv may return short; loop until the body is fully read. */
    while (got < want) {
        int n = httpd_req_recv(req, buf + got, want - got);
        if (n > 0) {
            got += (size_t)n;
            retries = 0;
        } else if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++retries > 5) break;
        } else {
            break;      /* connection closed or unrecoverable error */
        }
    }
    buf[got] = '\0';
    return (int)got;
}
static bool qarg(httpd_req_t *req, const char *key, char *out, size_t len)
{
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return false;
    return httpd_query_key_value(query, key, out, len) == ESP_OK;
}

/* Handlers that read or mutate the shared config take the global config lock
 * (recursive: storage_save_config etc. may re-enter). Every return path after
 * CFG_LOCK() must release it — use CFG_RET() instead of a bare return. */
#define CFG_LOCK() he_config_lock()
#define CFG_RET(x) do { he_config_unlock(); return (x); } while (0)

/* ---- Panel authentication (session cookie) ----
 * A random 32-byte token is generated on login and stored both in an
 * HttpOnly cookie and in a small RAM session table (TTL + max sessions).
 * Every /api handler except login/logout goes through auth_guard(), which
 * validates the cookie in constant time. Passwords live in NVS (system_config,
 * tail fields) and are compared with mbedtls_ct_memcmp to avoid timing leaks. */
#define HE_SESS_MAX     8
#define HE_SESS_TTL_MS  (24LL * 60 * 60 * 1000)   /* 24 h */
#define HE_SESS_COOKIE  "HE_SID"
#define HE_LOGIN_MAX_TRIES 3
#define HE_LOGIN_LOCK_MS   (2 * 60 * 1000)      /* 2 min lockout after N fails */

typedef struct {
    bool     used;
    uint8_t  token[32];
    int64_t  expires_ms;
} panel_session_t;

static panel_session_t s_sess[HE_SESS_MAX];
static SemaphoreHandle_t s_sess_mutex;
static int s_login_fails = 0;
static int64_t s_login_lock_until = 0;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char *hx = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = hx[in[i] >> 4];
        out[i * 2 + 1] = hx[in[i] & 0xF];
    }
    out[n * 2] = '\0';
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_hex(const char *in, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int hi = hex_val(in[i * 2]), lo = hex_val(in[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool session_create(uint8_t *token /* out, 32 B */)
{
    bool ok = false;
    xSemaphoreTake(s_sess_mutex, portMAX_DELAY);
    int64_t now = now_ms();
    /* Purge expired entries first so a free slot is usually found. */
    for (int i = 0; i < HE_SESS_MAX; i++)
        if (s_sess[i].used && s_sess[i].expires_ms <= now) s_sess[i].used = false;
    for (int i = 0; i < HE_SESS_MAX; i++) {
        if (!s_sess[i].used) {
            esp_fill_random(token, 32);
            memcpy(s_sess[i].token, token, 32);
            s_sess[i].expires_ms = now + HE_SESS_TTL_MS;
            s_sess[i].used = true;
            ok = true;
            break;
        }
    }
    xSemaphoreGive(s_sess_mutex);
    return ok;
}

static bool session_valid(const uint8_t *token)
{
    bool ok = false;
    xSemaphoreTake(s_sess_mutex, portMAX_DELAY);
    int64_t now = now_ms();
    for (int i = 0; i < HE_SESS_MAX; i++) {
        if (!s_sess[i].used || s_sess[i].expires_ms <= now) continue;
        volatile const uint8_t *a = s_sess[i].token, *b = token;
        volatile int diff = 0;
        for (int j = 0; j < 32; j++) diff |= a[j] ^ b[j];
        if (!diff) { s_sess[i].expires_ms = now + HE_SESS_TTL_MS; ok = true; break; }
    }
    xSemaphoreGive(s_sess_mutex);
    return ok;
}

static void session_destroy(const uint8_t *token)
{
    xSemaphoreTake(s_sess_mutex, portMAX_DELAY);
    for (int i = 0; i < HE_SESS_MAX; i++) {
        if (s_sess[i].used && memcmp(s_sess[i].token, token, 32) == 0) {
            s_sess[i].used = false;
            break;
        }
    }
    xSemaphoreGive(s_sess_mutex);
}

/* Constant-time string comparison (mbedtls_ct_memcmp needs size_t len). */
static bool ct_str_equal(const char *a, size_t alen, const char *b, size_t blen)
{
    /* Compare up to the shorter length in constant time, then require equal
     * lengths; a plain length check leaks nothing usable. */
    size_t n = alen < blen ? alen : blen;
    int diff = mbedtls_ct_memcmp(a, b, n) != 0;
    diff |= (alen != blen);
    return !diff;
}

static bool auth_enabled(void)
{
    return s_cfg && s_cfg->panel_user[0] != '\0';
}

/* Extract HE_SID cookie from the Cookie header. */
static bool get_session_cookie(httpd_req_t *req, uint8_t *token)
{
    char hdr[128];
    if (httpd_req_get_hdr_value_str(req, "Cookie", hdr, sizeof(hdr)) != ESP_OK) return false;
    const char *p = strstr(hdr, HE_SESS_COOKIE "=");
    if (!p) return false;
    p += strlen(HE_SESS_COOKIE "=");
    return parse_hex(p, token, 32);
}

/* Trampoline: wraps every protected handler with the session check. */
typedef esp_err_t (*handler_t)(httpd_req_t *);

static esp_err_t auth_guard(httpd_req_t *req)
{
    if (auth_enabled()) {
        uint8_t token[32];
        if (!get_session_cookie(req, token) || !session_valid(token)) {
            httpd_resp_set_status(req, "401 Unauthorized");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            return httpd_resp_send(req, "{\"auth\":false}", HTTPD_RESP_USE_STRLEN);
        }
    }
    handler_t h = (handler_t)req->user_ctx;
    return h(req);
}

/* ---- /api/login (POST: {"user","pass"}) ---- */
static esp_err_t h_login(httpd_req_t *req)
{
    char body[160]; read_body(req, body, sizeof(body));
    char user[16], pass[32];
    bool have_u = json_str(body, "user", user, sizeof(user));
    bool have_p = json_str(body, "pass", pass, sizeof(pass));
    if (!have_u || !have_p) return send_json(req, "{\"auth\":false,\"err\":\"missing credentials\"}");

    if (auth_enabled()) {
        /* Brute-force throttle: reject all attempts while a lockout is active
         * (HE_LOGIN_MAX_TRIES failures => HE_LOGIN_LOCK_MS of silence). */
        int64_t now = now_ms();
        if (now < s_login_lock_until) {
            httpd_resp_set_status(req, "429 Too Many Requests");
            return send_json(req, "{\"auth\":false,\"err\":\"too many attempts\"}");
        }
        bool ok = false;
        CFG_LOCK();
        ok = ct_str_equal(user, strnlen(user, sizeof(user)),
                          s_cfg->panel_user, strnlen(s_cfg->panel_user, sizeof(s_cfg->panel_user))) &&
             ct_str_equal(pass, strnlen(pass, sizeof(pass)),
                          s_cfg->panel_pass, strnlen(s_cfg->panel_pass, sizeof(s_cfg->panel_pass)));
        he_config_unlock();
        if (!ok) {
            int64_t fnow = now_ms();
            s_login_fails++;
            if (s_login_fails >= HE_LOGIN_MAX_TRIES) {
                s_login_lock_until = fnow + HE_LOGIN_LOCK_MS;
                s_login_fails = 0;          /* next window starts fresh */
            }
            httpd_resp_set_status(req, "401 Unauthorized");
            return send_json(req, "{\"auth\":false}");
        }
        s_login_fails = 0;
    }
    uint8_t token[32];
    if (!session_create(token)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return send_json(req, "{\"auth\":false,\"err\":\"no free session slot\"}");
    }
    char hex[65]; hex_encode(token, 32, hex);
    char cookie[144];
    int cn = snprintf(cookie, sizeof(cookie), "%s=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=86400", HE_SESS_COOKIE, hex);
    if (cn < 0 || (size_t)cn >= sizeof(cookie)) return send_json(req, "{\"auth\":false,\"err\":\"cookie\"}");
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return send_json(req, "{\"auth\":true}");
}

/* ---- /api/logout (POST) ---- */
static esp_err_t h_logout(httpd_req_t *req)
{
    uint8_t token[32];
    if (get_session_cookie(req, token)) session_destroy(token);
    httpd_resp_set_hdr(req, "Set-Cookie", HE_SESS_COOKIE "=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0");
    return send_json(req, "{\"auth\":false}");
}

/* ---- /api/password (POST: {"current","new"}) — change panel password ---- */
static esp_err_t h_password(httpd_req_t *req)
{
    if (!auth_enabled()) return send_text(req, "auth disabled", 400);
    char body[160]; read_body(req, body, sizeof(body));
    char cur[32], neu[32];
    if (!json_str(body, "current", cur, sizeof(cur)) || !json_str(body, "new", neu, sizeof(neu)))
        return send_text(req, "missing fields", 400);
    if (strnlen(neu, sizeof(neu)) < HE_PANEL_PASS_MIN)
        return send_text(req, "password too short", 400);

    bool ok = false;
    CFG_LOCK();
    ok = ct_str_equal(cur, strnlen(cur, sizeof(cur)),
                      s_cfg->panel_pass, strnlen(s_cfg->panel_pass, sizeof(s_cfg->panel_pass)));
    if (ok) {
        strncpy(s_cfg->panel_pass, neu, sizeof(s_cfg->panel_pass) - 1);
        s_cfg->panel_pass[sizeof(s_cfg->panel_pass) - 1] = '\0';
        storage_save_config(s_cfg);
    }
    he_config_unlock();
    if (!ok) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return send_text(req, "wrong password", 401);
    }
    return send_text(req, "ok", 200);
}

/* ---- static assets (gzip-embedded; browser decompresses) ---- */
static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    size_t n = index_html_end - index_html_start;
    return httpd_resp_send(req, (const char *)index_html_start, n);
}
static esp_err_t h_css(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    size_t n = style_css_end - style_css_start;
    return httpd_resp_send(req, (const char *)style_css_start, n);
}
static esp_err_t h_js(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    size_t n = app_js_end - app_js_start;
    return httpd_resp_send(req, (const char *)app_js_start, n);
}

/* ---- /api/state ---- */
/* JSON-escape a NUL-terminated string into `out` (capacity `outlen` incl. NUL).
 * Returns chars written (excl. NUL). User-controlled strings flow into JSON this
 * way so a " / \ / control char can't break the response or inject a key. (#3) */
static size_t json_escape(const char *in, char *out, size_t outlen)
{
    if (!outlen) return 0;
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 6 < outlen; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c == '\t') { out[o++] = '\\'; out[o++] = 't'; }
        else if (c == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (c < 0x20)  { o += snprintf(out + o, outlen - o, "\\u%04x", c); }
        else                { out[o++] = (char)c; }   /* printable incl. UTF-8 */
    }
    out[o] = '\0';
    return o;
}

/* Strip JSON/SMTP metacharacters (", \, control <0x20) from a user-supplied name
 * so it is safe to emit into JSON and to splice into an SMTP body/subject. Keeps
 * printable text incl. UTF-8. In-place (in==out) is safe (o <= i). Returns true
 * if the result is non-empty. (#3 -- also closes the sensor-name -> restart-email
 * body SMTP-injection gap that B5's device_name-only assumption missed.) */
static bool sanitize_name(const char *in, char *out, size_t outlen)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < outlen; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\' || c < 0x20) continue;
        out[o++] = (char)c;
    }
    out[o] = '\0';
    return out[0] != '\0';
}
static const char *sname(control_state_t s)
{
    switch (s) {
    case ST_IDLE: return "IDLE"; case ST_HEATING: return "HEATING";
    case ST_BOOST_5MIN: return "BOOST_5MIN"; case ST_PUMP_OVERRUN: return "PUMP_OVERRUN";
    case ST_FAULT: return "FAULT"; case ST_EMERGENCY_CYCLIC: return "EMERGENCY_CYCLIC";
    case ST_SIMULATION: return "SIMULATION"; default: return "?";
    }
}
static const char *fname(fault_class_t f)
{
    switch (f) {
    case FAULT_NONE: return "NONE"; case FAULT_SENSOR: return "SENSOR";
    case FAULT_SENSOR_IFACE: return "SENSOR_IFACE"; case FAULT_NO_HEAT_RISE: return "NO_HEAT_RISE";
    case FAULT_LOW_HEAT_RISE: return "LOW_HEAT_RISE"; case FAULT_NETWORK: return "NETWORK";
    case FAULT_STORAGE: return "STORAGE"; case FAULT_RESTART: return "RESTART"; default: return "?";
    }
}

static esp_err_t h_state(httpd_req_t *req)
{
    CFG_LOCK();
    system_snapshot_t snap; control_get_snapshot(&snap);
    size_t fs_total = 0, fs_used = 0; storage_fs_usage(&fs_total, &fs_used);
    storage_flash_wear_t fw; storage_get_flash_wear(&fw);
    char b[3200]; int p = 0;
    char edname[HE_NAME_LEN * 6 + 1];
    json_escape(s_cfg->device_name[0] ? s_cfg->device_name : HE_DEFAULT_DEVICE_NAME,
                edname, sizeof(edname));
    int64_t now_us = esp_timer_get_time();
    int64_t now_unix = (int64_t)time(NULL);
    p += snprintf(b + p, sizeof(b) - p,
        "{\"state\":\"%s\",\"heating\":%s,\"health\":%s,\"system\":%.2f,"
        "\"external\":%.2f,\"fault\":\"%s\",\"uptime\":%u,\"sim\":%s,"
        "\"disabled\":%s,\"boost\":%s,\"net_up\":%s,\"storage_ok\":%s,"
        "\"fs_used\":%u,\"fs_total\":%u,"
        "\"flash_wear_pct\":%.3f,\"flash_erase_cycles\":%lu,"
        "\"flash_nvs_writes\":%lu,\"flash_fs_kb\":%lu,"
        "\"fault_grace_sec\":%d,\"max_on_sec\":%d,\"max_on_break_sec\":%d,"
        "\"min_on_sec\":%d,\"min_off_sec\":%d,\"anti_osc_lock_sec\":%d,"
        "\"time_synced\":%s,\"time_now\":%lld,\"device_ip\":\"%s\","
        "\"sensors\":[",
        sname(snap.state), snap.heating_active ? "true" : "false",
        snap.health_ok ? "true" : "false",
        he_isnan(snap.system_temp) ? -99.0f : snap.system_temp,
        he_isnan(snap.external_temp) ? -99.0f : snap.external_temp,
        fname(snap.active_fault), (unsigned)snap.uptime_ms,
        snap.simulation_mode ? "true" : "false",
        control_heating_disabled() ? "true" : "false",
        control_boost_active() ? "true" : "false",
        network_is_up() ? "true" : "false",
        storage_healthy() ? "true" : "false",
        (unsigned)fs_used, (unsigned)fs_total,
        (double)fw.est_erase_pct, (unsigned long)fw.est_erase_cycles,
        (unsigned long)fw.nvs_commits, (unsigned long)fw.fs_kb_written,
        s_cfg->fault_grace_sec, s_cfg->max_on_sec, s_cfg->max_on_break_sec,
        s_cfg->min_on_sec, s_cfg->min_off_sec, s_cfg->anti_osc_lock_sec,
        he_time_valid() ? "true" : "false",
        (long long)now_unix,
        network_device_ip());

    for (int i = 0; i < s_cfg->sensor_count && p + 120 < (int)sizeof(b); i++) {
        sensor_t *s = &s_cfg->sensors[i];
        int age_s = s->last_update_ms ? (int)((now_us / 1000 - s->last_update_ms) / 1000) : 999;
        char ename[HE_NAME_LEN * 6 + 1];
        json_escape(s->name, ename, sizeof(ename));
        p += snprintf(b + p, sizeof(b) - p,
            "%s{\"id\":%d,\"name\":\"%s\",\"active\":%s,\"weight\":%.3f,"
            "\"calib\":%.2f,\"comfort\":%.2f,\"quality\":\"%s\",\"eff\":%.2f,"
            "\"raw\":%.2f,\"sim\":%s,\"window\":%s,\"last_seen\":%d}",
            i ? "," : "", s->id, ename, s->active ? "true" : "false",
            s->weight, s->calib_offset, s->comfort_offset, sensor_quality_name(s->quality),
            he_isnan(s->last_effective) ? -99.0f : s->last_effective,
            he_isnan(s->last_raw) ? -99.0f : s->last_raw,
            s->simulated ? "true" : "false", s->window_open ? "true" : "false",
            age_s);
    }
    if (s_cfg->has_external) {
        sensor_t *e = &s_cfg->sensors[HE_MAX_SENSORS];
        int e_age = e->last_update_ms ? (int)((now_us / 1000 - e->last_update_ms) / 1000) : 999;
        char ename[HE_NAME_LEN * 6 + 1];
        json_escape(e->name, ename, sizeof(ename));
        p += snprintf(b + p, sizeof(b) - p,
            ",{\"id\":0,\"name\":\"%s\",\"active\":%s,\"weight\":0,\"calib\":0,"
            "\"comfort\":0,\"quality\":\"%s\",\"eff\":%.2f,\"raw\":%.2f,"
            "\"sim\":%s,\"window\":false,\"external\":true,\"last_seen\":%d}",
            ename, e->active ? "true" : "false", sensor_quality_name(e->quality),
            he_isnan(e->last_effective) ? -99.0f : e->last_effective,
            he_isnan(e->last_raw) ? -99.0f : e->last_raw,
            e->simulated ? "true" : "false", e_age);
    }
    p += snprintf(b + p, sizeof(b) - p, "],");
    /* profile + modes */
    char prof[600]; profile_to_json(&s_cfg->profile, prof, sizeof(prof));
    p += snprintf(b + p, sizeof(b) - p,
        "\"profile\":%s,\"pump\":{\"enabled\":%s,\"impulse\":%d,\"period\":%d,\"total\":%d},"
        "\"emergency\":{\"enabled\":%s,\"on\":%d,\"period\":%d,\"on_fault\":%s},"
        "\"sim_sensors\":%s,\"sim_heating\":%s,\"sim_accel\":%s,\"device_name\":\"%s\","
        "\"notify_ev\":{\"faults\":%s,\"restart\":%s}",
        prof,
        s_cfg->pump.enabled ? "true" : "false", s_cfg->pump.impulse_seconds,
        s_cfg->pump.period_seconds, s_cfg->pump.total_seconds,
        s_cfg->emergency.enabled ? "true" : "false", s_cfg->emergency.on_seconds,
        s_cfg->emergency.period_seconds,
        s_cfg->emergency_on_sensor_fault ? "true" : "false",
        s_cfg->simulate_sensors ? "true" : "false",
        s_cfg->simulate_heating ? "true" : "false",
        s_cfg->sim_time_accel ? "true" : "false",
        edname,
        s_cfg->notify_ev_faults ? "true" : "false",
        s_cfg->notify_ev_restart ? "true" : "false");
    /* Echo the current network + notification settings so the UI can show them
     * (SSID/mode in the Sieć card, recipients/servers in Powiadomienia). Secrets
     * (wifi_pass, smtp_pass) are deliberately NOT emitted — the UI leaves those
     * fields empty and the save handlers treat an empty password as "keep
     * current". One shared escape buffer (largest field is 64 -> 64*6+1) keeps
     * the stack footprint small; httpd stack is 8192. */
    char ebuf[64 * 6 + 1];
    p += snprintf(b + p, sizeof(b) - p,
        ",\"wifi\":{\"sta_mode\":%s,\"ssid\":\"",
        s_cfg->wifi_sta_mode ? "true" : "false");
    json_escape(s_cfg->wifi_ssid, ebuf, sizeof(ebuf));
    p += snprintf(b + p, sizeof(b) - p, "%s\"},\"notify\":{\"email_enabled\":%s,"
        "\"sms_enabled\":%s,\"email_to\":\"", ebuf,
        s_cfg->notify.email_enabled ? "true" : "false",
        s_cfg->notify.sms_enabled ? "true" : "false");
    json_escape(s_cfg->notify.email_to, ebuf, sizeof(ebuf));
    p += snprintf(b + p, sizeof(b) - p, "%s\",\"smtp_host\":\"", ebuf);
    json_escape(s_cfg->notify.smtp_host, ebuf, sizeof(ebuf));
    p += snprintf(b + p, sizeof(b) - p, "%s\",\"smtp_port\":%d,\"smtp_user\":\"",
                 ebuf, s_cfg->smtp_port);
    json_escape(s_cfg->notify.smtp_user, ebuf, sizeof(ebuf));
    p += snprintf(b + p, sizeof(b) - p, "%s\",\"sms_phone\":\"", ebuf);
    json_escape(s_cfg->notify.sms_phone, ebuf, sizeof(ebuf));
    p += snprintf(b + p, sizeof(b) - p, "%s\",\"sms_gateway\":\"", ebuf);
    json_escape(s_cfg->notify.sms_gateway, ebuf, sizeof(ebuf));
    p += snprintf(b + p, sizeof(b) - p, "%s\"}}", ebuf);
    CFG_RET(send_json(req, b));
}

/* ---- /api/sensor (POST: update one sensor) ---- */
static esp_err_t h_sensor_post(httpd_req_t *req)
{
    char idstr[8]; if (!qarg(req, "id", idstr, sizeof(idstr))) return send_text(req, "missing id", 400);
    int id = atoi(idstr);
    char body[512]; read_body(req, body, sizeof(body));

    CFG_LOCK();
    sensor_t *target = NULL;
    if (id == 0) target = s_cfg->has_external ? &s_cfg->sensors[HE_MAX_SENSORS] : NULL;
    else for (int i = 0; i < s_cfg->sensor_count; i++) if (s_cfg->sensors[i].id == id) { target = &s_cfg->sensors[i]; break; }
    if (!target) CFG_RET(send_text(req, "no such sensor", 400));

    char name[HE_NAME_LEN]; if (json_str(body, "name", name, sizeof(name)) && sanitize_name(name, name, sizeof(name))) { strncpy(target->name, name, sizeof(target->name)-1); target->name[sizeof(target->name)-1]='\0'; }
    bool b; float f;
    if (json_bool(body, "active", &b)) target->active = b;
    if (json_float(body, "weight", &f)) target->weight = he_clampf(f, 0.0f, 1.0f);
    if (json_float(body, "calib", &f)) target->calib_offset = he_clampf(f, -20.0f, 20.0f);
    if (json_float(body, "comfort", &f)) target->comfort_offset = he_clampf(f, -20.0f, 20.0f);
    storage_save_config(s_cfg);
    CFG_RET(send_text(req, "ok", 200));
}

/* ---- /api/sensor/restore?id= ---- */
static esp_err_t h_sensor_restore(httpd_req_t *req)
{
    char idstr[8]; if (!qarg(req, "id", idstr, sizeof(idstr))) return send_text(req, "missing id", 400);
    CFG_LOCK();
    sensor_manager_restore(atoi(idstr));
    CFG_RET(send_text(req, "ok", 200));
}

/* ---- /api/sensors/count?n=  (define how many internal sensors, 1..6) ---- */
static esp_err_t h_sensors_count(httpd_req_t *req)
{
    char ns[8]; if (!qarg(req, "n", ns, sizeof(ns))) return send_text(req, "missing n", 400);
    int n = atoi(ns);
    if (n < 1) n = 1;
    if (n > HE_MAX_SENSORS) n = HE_MAX_SENSORS;
    CFG_LOCK();
    int prev = s_cfg->sensor_count;
    s_cfg->sensor_count = n;
    /* Initialise any newly added sensors with sane defaults. */
    for (int i = prev; i < n; i++) {
        sensor_t *s = &s_cfg->sensors[i];
        memset(s, 0, sizeof(*s));
        s->id = (uint8_t)(i + 1);
        snprintf(s->name, sizeof(s->name), "Pomieszczenie %d", i + 1);
        s->active = true;
        s->weight = 1.0f / (float)n;
        s->sim_src = SIM_SRC_REAL;
        s->quality = QUAL_TIMEOUT;
    }
    /* Re-normalise weights so they sum to 1 across active sensors. */
    float wsum = 0; for (int i = 0; i < n; i++) wsum += s_cfg->sensors[i].weight;
    if (wsum > 0) for (int i = 0; i < n; i++) s_cfg->sensors[i].weight /= wsum;
    sensor_manager_bind(s_cfg->sensors, s_cfg->sensor_count,
                        s_cfg->has_external ? &s_cfg->sensors[HE_MAX_SENSORS] : NULL);
    storage_save_config(s_cfg);
    CFG_RET(send_text(req, "ok", 200));
}

/* ---- /api/profile (GET/POST) ---- */
static esp_err_t h_profile_get(httpd_req_t *req)
{
    CFG_LOCK();
    char b[600]; profile_to_json(&s_cfg->profile, b, sizeof(b));
    CFG_RET(send_json(req, b));
}
static esp_err_t h_profile_post(httpd_req_t *req)
{
    char body[700]; read_body(req, body, sizeof(body));
    daily_profile_t p;
    if (!profile_from_json(&p, body, strlen(body))) return send_text(req, "bad profile json", 400);
    int eh; char em[48];
    if (!profile_validate(&p, &eh, em, sizeof(em))) return send_text(req, em, 400);
    CFG_LOCK();
    s_cfg->profile = p;
    storage_save_config(s_cfg);
    CFG_RET(send_text(req, "ok", 200));
}

/* ---- /api/profile/file?name=&op=save|load ---- */
static esp_err_t h_profile_file(httpd_req_t *req)
{
    char name[32], op[8];
    if (!qarg(req, "name", name, sizeof(name))) return send_text(req, "missing name", 400);
    if (!qarg(req, "op", op, sizeof(op))) strcpy(op, "load");

    if (strcmp(op, "save") == 0) {
        /* Read & validate the body BEFORE taking the config lock (mirrors
         * h_profile_post). blen==0 is back-compat: persist the active profile.
         * blen>0 with a parse/validate failure is a real error -> 400, NOT a
         * silent fallback to the active profile (which would wipe the slot). */
        char body[700];
        int blen = read_body(req, body, sizeof(body));
        daily_profile_t p;
        bool use_active = (blen <= 0);
        if (!use_active) {
            if (!profile_from_json(&p, body, blen)) return send_text(req, "bad profile json", 400);
            int eh; char em[48];
            if (!profile_validate(&p, &eh, em, sizeof(em))) return send_text(req, em, 400);
        }
        CFG_LOCK();
        esp_err_t sv = storage_save_profile_file(name, use_active ? &s_cfg->profile : &p);
        CFG_RET(sv == ESP_OK ? send_text(req, "ok", 200) : send_text(req, "save failed", 500));
    }

    /* load: read & validate the file before locking to mutate the active profile. */
    daily_profile_t p;
    if (storage_load_profile_file(name, &p) != ESP_OK) return send_text(req, "load failed", 500);
    int eh; char em[48];
    if (!profile_validate(&p, &eh, em, sizeof(em))) return send_text(req, "invalid profile file", 400);
    CFG_LOCK();
    s_cfg->profile = p;
    storage_save_config(s_cfg);
    CFG_RET(send_text(req, "ok", 200));
}

/* ---- /api/boost?on=1 ---- */
static esp_err_t h_boost(httpd_req_t *req)
{
    char on[8]; if (qarg(req, "on", on, sizeof(on))) { CFG_LOCK(); control_request_boost(atoi(on) == 1); he_config_unlock(); }
    return send_text(req, "ok", 200);
}

/* ---- /api/heating?disable=1 ---- */
static esp_err_t h_heating(httpd_req_t *req)
{
    char d[8]; if (qarg(req, "disable", d, sizeof(d))) { CFG_LOCK(); control_set_heating_disabled(atoi(d) == 1); he_config_unlock(); }
    return send_text(req, "ok", 200);
}

/* ---- /api/device (POST) ---- */
static esp_err_t h_device(httpd_req_t *req)
{
    char body[64]; read_body(req, body, sizeof(body));
    char name[32];
    if (!json_str(body, "name", name, sizeof(name))) return send_text(req, "missing name", 400);
    /* Strip JSON/SMTP metacharacters so the name can't break the /api/state JSON
     * or inject SMTP headers when it lands in a notification subject/body. Keeps
     * printable text incl. UTF-8. Reject if nothing printable remains. (#3) */
    if (!sanitize_name(name, name, sizeof(name))) return send_text(req, "invalid name", 400);

    CFG_LOCK();
    strncpy(s_cfg->device_name, name, sizeof(s_cfg->device_name) - 1);
    s_cfg->device_name[sizeof(s_cfg->device_name) - 1] = '\0';
    storage_save_config(s_cfg);
    he_config_unlock();
    return send_text(req, "ok", 200);
}

/* ---- /api/pump (POST) ---- */
static esp_err_t h_pump(httpd_req_t *req)
{
    char body[256]; read_body(req, body, sizeof(body));
    bool en; int i;
    CFG_LOCK();
    if (json_bool(body, "enabled", &en)) s_cfg->pump.enabled = en;
    if (json_int(body, "impulse", &i)) s_cfg->pump.impulse_seconds = i;
    if (json_int(body, "period", &i)) s_cfg->pump.period_seconds = i;
    if (json_int(body, "total", &i)) s_cfg->pump.total_seconds = i;
    storage_save_config(s_cfg);
    CFG_RET(send_text(req, "ok", 200));
}

/* ---- /api/emergency (POST) ---- */
static esp_err_t h_emergency(httpd_req_t *req)
{
    char body[256]; read_body(req, body, sizeof(body));
    bool en; int i;
    CFG_LOCK();
    if (json_bool(body, "enabled", &en)) s_cfg->emergency.enabled = en;
    if (json_int(body, "on", &i)) s_cfg->emergency.on_seconds = i;
    if (json_int(body, "period", &i)) s_cfg->emergency.period_seconds = i;
    if (json_bool(body, "on_fault", &en)) s_cfg->emergency_on_sensor_fault = en;
    storage_save_config(s_cfg);
    CFG_RET(send_text(req, "ok", 200));
}

/* ---- /api/notify/events (POST: per-event-type e-mail subscription) ----
 * Separate from /api/notify so toggling these does not touch (and blank) the
 * SMTP/SMS fields the existing btnNotify save writes, and so we can stamp the
 * notify_ev_ver sentinel here to protect the user's choice from a future
 * repair_config re-default. */
static esp_err_t h_notify_events(httpd_req_t *req)
{
    char body[128]; read_body(req, body, sizeof(body));
    bool en;
    CFG_LOCK();
    if (json_bool(body, "faults", &en))  s_cfg->notify_ev_faults  = en;
    if (json_bool(body, "restart", &en)) s_cfg->notify_ev_restart = en;
    s_cfg->notify_ev_ver = 1;   /* user has configured -> protect from future re-default */
    storage_save_config(s_cfg);
    CFG_RET(send_text(req, "ok", 200));
}

/* ---- /api/notify (POST) ---- */
static esp_err_t h_notify(httpd_req_t *req)
{
    char body[512]; read_body(req, body, sizeof(body));
    bool en; char tmp[32];
    CFG_LOCK();
    if (json_bool(body, "email_enabled", &en)) s_cfg->notify.email_enabled = en;
    if (json_bool(body, "sms_enabled", &en)) s_cfg->notify.sms_enabled = en;
    json_str(body, "email_to", s_cfg->notify.email_to, sizeof(s_cfg->notify.email_to));
    json_str(body, "smtp_host", s_cfg->notify.smtp_host, sizeof(s_cfg->notify.smtp_host));
    json_str(body, "smtp_user", s_cfg->notify.smtp_user, sizeof(s_cfg->notify.smtp_user));
    int p;
    if (json_int(body, "smtp_port", &p)) {
        if (p < 1 || p > 65535) p = HE_DEFAULT_SMTP_PORT;
        s_cfg->smtp_port = p;
    }
    /* The UI does not echo the SMTP password, so an empty value here means
     * "keep the stored password" — only overwrite when a new one is typed. */
    if (json_str(body, "smtp_pass", tmp, sizeof(tmp)) && tmp[0]) {
        strncpy(s_cfg->notify.smtp_pass, tmp, sizeof(s_cfg->notify.smtp_pass) - 1);
        s_cfg->notify.smtp_pass[sizeof(s_cfg->notify.smtp_pass) - 1] = '\0';
    }
    json_str(body, "sms_phone", s_cfg->notify.sms_phone, sizeof(s_cfg->notify.sms_phone));
    json_str(body, "sms_gateway", s_cfg->notify.sms_gateway, sizeof(s_cfg->notify.sms_gateway));
    storage_save_config(s_cfg);
    CFG_RET(send_text(req, "ok", 200));
}

/* ---- /api/notify/test (POST) ---- */
static esp_err_t h_notify_test(httpd_req_t *req)
{
    CFG_LOCK();
    notify_cfg_t cfg_copy = s_cfg->notify;  /* snapshot under lock */
    int port_copy = s_cfg->smtp_port;
    he_config_unlock();
    char *diag = notification_test_email(&cfg_copy, port_copy);
    if (!diag) { send_text(req, "internal error", 500); return ESP_FAIL; }
    /* Return as JSON so the frontend can display it nicely. */
    char buf[1200];
    int n = snprintf(buf, sizeof(buf), "{\"result\":\"%s\"}", diag);
    free(diag);
    /* Escape any literal quotes in the diag for JSON safety. */
    for (int i = 0; i < n && i < (int)sizeof(buf); i++) {
        if (buf[i] == '\n') buf[i] = ' ';  /* single-line JSON */
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

/* ---- /api/network (POST) ---- */
static esp_err_t h_network(httpd_req_t *req)
{
    char body[512]; read_body(req, body, sizeof(body));
    bool sta;
    char tmp[64];
    CFG_LOCK();
    if (json_bool(body, "sta_mode", &sta)) s_cfg->wifi_sta_mode = sta;
    /* Treat an empty ssid/pass as "leave unchanged": the UI now echoes the
     * current SSID (not the password), so an empty pass on save must NOT blank
     * the stored credential. Only overwrite when the client sends a non-empty
     * value (i.e. the user typed a new one). */
    if (json_str(body, "ssid", tmp, sizeof(tmp)) && tmp[0]) {
        strncpy(s_cfg->wifi_ssid, tmp, sizeof(s_cfg->wifi_ssid) - 1);
        s_cfg->wifi_ssid[sizeof(s_cfg->wifi_ssid) - 1] = '\0';
    }
    if (json_str(body, "pass", tmp, sizeof(tmp)) && tmp[0]) {
        strncpy(s_cfg->wifi_pass, tmp, sizeof(s_cfg->wifi_pass) - 1);
        s_cfg->wifi_pass[sizeof(s_cfg->wifi_pass) - 1] = '\0';
    }
    storage_save_config(s_cfg);
    network_apply(s_cfg);
    CFG_RET(send_text(req, "ok", 200));
}

/* ---- /api/sim/sensor (POST) ---- */
static esp_err_t h_sim_sensor(httpd_req_t *req)
{
    char idstr[8]; if (!qarg(req, "id", idstr, sizeof(idstr))) return send_text(req, "missing id", 400);
    int id = atoi(idstr);
    char body[256]; read_body(req, body, sizeof(body));
    CFG_LOCK();
    sensor_t *t = NULL;
    if (id == 0) t = s_cfg->has_external ? &s_cfg->sensors[HE_MAX_SENSORS] : NULL;
    else for (int i = 0; i < s_cfg->sensor_count; i++) if (s_cfg->sensors[i].id == id) { t = &s_cfg->sensors[i]; break; }
    if (!t) CFG_RET(send_text(req, "no sensor", 400));

    int src; float base = 20, rate = 0, target = 0;
    if (json_int(body, "src", &src)) {
        sim_source_t ss = (sim_source_t)src;
        json_float(body, "base", &base); json_float(body, "rate", &rate); json_float(body, "target", &target);
        if (ss == SIM_SRC_REAL) { t->simulated = false; t->sim_src = SIM_SRC_REAL; }
        else sim_set_sensor_source(t, ss, base, rate, target);
        s_cfg->simulate_sensors = sim_any_sensor_simulated(s_cfg->sensors, s_cfg->sensor_count) ||
                                  (s_cfg->has_external && s_cfg->sensors[HE_MAX_SENSORS].simulated);
    }
    storage_save_config(s_cfg);
    CFG_RET(send_text(req, "ok", 200));
}

/* ---- /api/sim/heating (POST) ---- */
static esp_err_t h_sim_heating(httpd_req_t *req)
{
    char body[256]; read_body(req, body, sizeof(body));
    bool en, mixed = false; int mode = 0; float hr = 0, cr = 0, in = 0;
    CFG_LOCK();
    if (json_bool(body, "enabled", &en)) {
        s_cfg->simulate_heating = en;
        json_bool(body, "mixed", &mixed);
        heating_output_set_simulation(en, mixed);
        /* Closed-loop no-hardware demo (spec 8): drive active sensors from the
         * thermal model so the controller, fault manager and chart all observe a
         * live system temperature. Sensors still REAL are switched to VIRTUAL;
         * on leaving simulation, the VIRTUAL ones revert to REAL. */
        for (int i = 0; i < s_cfg->sensor_count; i++) {
            sensor_t *s = &s_cfg->sensors[i];
            if (!s->active) continue;
            if (en && s->sim_src == SIM_SRC_REAL)
                sim_set_sensor_source(s, SIM_SRC_VIRTUAL,
                    he_isnan(s->last_effective) ? 20.0f : s->last_effective, 0, 0);
            else if (!en && s->sim_src == SIM_SRC_VIRTUAL)
                sim_set_sensor_source(s, SIM_SRC_REAL, 0, 0, 0);
        }
        if (s_cfg->has_external) {
            sensor_t *e = &s_cfg->sensors[HE_MAX_SENSORS];
            if (en && e->sim_src == SIM_SRC_REAL)
                sim_set_sensor_source(e, SIM_SRC_CONST, 5.0f, 0, 0);
            else if (!en && e->sim_src == SIM_SRC_CONST)
                sim_set_sensor_source(e, SIM_SRC_REAL, 0, 0, 0);
        }
        s_cfg->simulate_sensors =
            sim_any_sensor_simulated(s_cfg->sensors, s_cfg->sensor_count) ||
            (s_cfg->has_external && s_cfg->sensors[HE_MAX_SENSORS].simulated);
    }
    if (json_int(body, "mode", &mode)) {
        json_float(body, "heat_rate", &hr); json_float(body, "cool_rate", &cr); json_float(body, "inertia", &in);
        sim_set_heating_mode((heating_sim_mode_t)mode, hr, cr, in);
    }
    bool accel;
    if (json_bool(body, "accel", &accel)) s_cfg->sim_time_accel = accel;
    storage_save_config(s_cfg);
    CFG_RET(send_text(req, "ok", 200));
}

/* ---- /api/fault/clear ---- */
static esp_err_t h_fault_clear(httpd_req_t *req)
{
    CFG_LOCK();
    control_clear_fault();
    he_config_unlock();
    return send_text(req, "ok", 200);
}

/* ---- /api/limits (POST) ---- */
static esp_err_t h_limits(httpd_req_t *req)
{
    char body[256]; read_body(req, body, sizeof(body));
    int v;
    CFG_LOCK();
    if (json_int(body, "fault_grace_sec", &v) && v >= 60 && v <= 3600)
        s_cfg->fault_grace_sec = v;
    if (json_int(body, "max_on_sec", &v) && v >= 300 && v <= 86400)
        s_cfg->max_on_sec = v;
    if (json_int(body, "max_on_break_sec", &v) && v >= 60 && v <= 86400)
        s_cfg->max_on_break_sec = v;
    /* User-configurable hysteresis timing (anti-chatter floor enforced). */
    if (json_int(body, "min_on_sec", &v) && v >= HE_MIN_ON_OFF_FLOOR && v <= 3600)
        s_cfg->min_on_sec = v;
    if (json_int(body, "min_off_sec", &v) && v >= HE_MIN_ON_OFF_FLOOR && v <= 3600)
        s_cfg->min_off_sec = v;
    if (json_int(body, "anti_osc_lock_sec", &v) && v >= HE_ANTIOSC_FLOOR && v <= 600)
        s_cfg->anti_osc_lock_sec = v;
    storage_save_config(s_cfg);
    CFG_RET(send_text(req, "ok", 200));
}

/* ---- chunked history endpoints ---- */

#define SAMPLE_BATCH  40   /* samples per ring-buffer copy (avoids large BSS) */

static esp_err_t h_samples(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    int total = storage_ring_count();
    httpd_resp_send_chunk(req, "[", 1);

    char line[260];
    minute_sample_t batch[SAMPLE_BATCH];
    int sent = 0;

    for (int pos = 0; pos < total; pos += SAMPLE_BATCH) {
        int n = storage_ring_copy(pos, SAMPLE_BATCH, batch);
        for (int i = 0; i < n; i++) {
            int L = snprintf(line, sizeof(line),
                "%s[%ld,%.2f,%.2f,%u,%u", sent ? "," : "",
                (long)batch[i].ts,
                he_isnan(batch[i].system_temp) ? -99.0f : batch[i].system_temp,
                he_isnan(batch[i].external_temp) ? -99.0f : batch[i].external_temp,
                batch[i].heating_active, batch[i].state);
            for (int s = 0; s < HE_MAX_SENSORS; s++)
                L += snprintf(line + L, sizeof(line) - L, ",%.2f",
                              he_isnan(batch[i].per_sensor[s]) ? -99.0f : batch[i].per_sensor[s]);
            L += snprintf(line + L, sizeof(line) - L, "]");
            if (httpd_resp_send_chunk(req, line, L) != ESP_OK) {
                httpd_resp_send_chunk(req, NULL, 0);
                return ESP_FAIL;
            }
            sent++;
        }
    }
    httpd_resp_send_chunk(req, "]", 1);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t h_daily(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    int n = 0;
    xSemaphoreTake(s_hist_mutex, portMAX_DELAY);
    storage_read_daily_aggregates(13, s_daily, 400, &n);

    /* Today's key (0 when wall-clock time is not yet valid). The historical
     * daily.csv row for today (if any) is skipped and replaced by the live
     * aggregate below -- today is always the right-most, live-updating column. */
    int today_key = 0;
    minute_sample_t cur;
    bool have_cur = storage_read_daily_current(&cur, &today_key);

    httpd_resp_send_chunk(req, "[", 1);
    char line[80];
    bool first = true;
    for (int i = 0; i < n; i++) {
        if (today_key && s_daily[i].ts == today_key) continue;   /* superseded by live */
        int L = snprintf(line, sizeof(line), "%s[%d,%.2f,%.2f,%u]", first ? "" : ",",
                         (int)s_daily[i].ts,
                         he_isnan(s_daily[i].system_temp) ? -99.0f : s_daily[i].system_temp,
                         he_isnan(s_daily[i].external_temp) ? -99.0f : s_daily[i].external_temp,
                         (unsigned)s_daily[i].heat_mins);
        if (httpd_resp_send_chunk(req, line, L) != ESP_OK) { xSemaphoreGive(s_hist_mutex); httpd_resp_send_chunk(req, NULL, 0); return ESP_FAIL; }
        first = false;
    }
    /* Append the live aggregate for the current day (right-most column). */
    if (have_cur) {
        int L = snprintf(line, sizeof(line), "%s[%d,%.2f,%.2f,%u]", first ? "" : ",",
                         (int)cur.ts,
                         he_isnan(cur.system_temp) ? -99.0f : cur.system_temp,
                         he_isnan(cur.external_temp) ? -99.0f : cur.external_temp,
                         (unsigned)cur.heat_mins);
        if (httpd_resp_send_chunk(req, line, L) != ESP_OK) { xSemaphoreGive(s_hist_mutex); httpd_resp_send_chunk(req, NULL, 0); return ESP_FAIL; }
    }
    xSemaphoreGive(s_hist_mutex);
    httpd_resp_send_chunk(req, "]", 1);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t h_log(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    int n = 0;
    xSemaphoreTake(s_hist_mutex, portMAX_DELAY);
    storage_read_log(s_logbuf, 100, &n);
    httpd_resp_send_chunk(req, "[", 1);
    char line[180];
    for (int i = 0; i < n; i++) {
        int L = snprintf(line, sizeof(line), "%s[%ld,%u,%u,\"%s\"]", i ? "," : "",
                         (long)s_logbuf[i].ts, (unsigned)s_logbuf[i].fault, (unsigned)s_logbuf[i].severity,
                         s_logbuf[i].text);
        if (httpd_resp_send_chunk(req, line, L) != ESP_OK) { xSemaphoreGive(s_hist_mutex); httpd_resp_send_chunk(req, NULL, 0); return ESP_FAIL; }
    }
    xSemaphoreGive(s_hist_mutex);
    httpd_resp_send_chunk(req, "]", 1);
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ---- /api/log/clear (POST) ---- */
static esp_err_t h_log_clear(httpd_req_t *req)
{
    storage_clear_log();
    return send_text(req, "ok", 200);
}

/* ---- /api/lora/test — E32 module communication check ----
 * Per the E32-433T20D datasheet (mode 3, M0=M1=1, 9600 8N1): three C1 bytes
 * read the module's saved parameters; it answers C0 + 5 bytes. The lora_rx
 * task is paused so it cannot swallow the response. Also probes the module
 * TXD level seen on our RX pin (GPIO14): HIGH = idle UART present (wiring
 * to the module OK), floating/LOW = broken link. */
static esp_err_t h_lora_test(httpd_req_t *req)
{
    char b[640];
    int p = 0;
    lora_receiver_test_mode(true);

    /* Probe: idle UART line from the module should read HIGH (3.3 V). */
    gpio_config_t gp = {
        .pin_bit_mask = 1ULL << HE_LORA_UART_RX,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&gp);
    int idle = gpio_get_level(HE_LORA_UART_RX);

    /* Mode pins to config (mode 3). */
    gpio_set_level(HE_GPIO_LORA_AUX,  HE_LORA_MODE_CONFIG);
    gpio_set_level(HE_GPIO_LORA_TXRX, HE_LORA_MODE_CONFIG);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* Flush stale RX bytes, send the read-parameters command. */
    uart_flush_input(HE_LORA_UART);
    const uint8_t cmd[3] = { 0xC1, 0xC1, 0xC1 };
    uart_write_bytes(HE_LORA_UART, cmd, sizeof(cmd));
    vTaskDelay(pdMS_TO_TICKS(300));

    uint8_t rsp[16] = {0};
    int n = uart_read_bytes(HE_LORA_UART, rsp, sizeof(rsp), pdMS_TO_TICKS(500));

    /* Restore normal mode (mode 0, listening) and hand the UART back. */
    gpio_set_level(HE_GPIO_LORA_AUX,  HE_LORA_MODE_NORMAL);
    gpio_set_level(HE_GPIO_LORA_TXRX, HE_LORA_MODE_NORMAL);
    lora_receiver_test_mode(false);

    p += snprintf(b, sizeof(b), "{\"lora\":{\"rx_idle\":%d,", idle);
    if (n >= 6 && rsp[0] == 0xC0) {
        /* C0 ADDH ADDL SPED CHAN OPTION — decode per datasheet §7.5. */
        uint8_t sped = rsp[3], chan = rsp[4], opt = rsp[5];
        static const int bps_tab[8] = {1200,2400,4800,9600,19200,38400,57600,115200};
        int baud = bps_tab[(sped >> 3) & 7];
        const char *parity = ((sped >> 6) & 3) == 0 ? "8N1" :
                             ((sped >> 6) & 3) == 1 ? "8O1" :
                             ((sped >> 6) & 3) == 2 ? "8E1" : "8N1";
        static const char *air_tab[8] = {"2.4k","2.4k","2.4k","4.8k","9.6k","19.2k","19.2k","19.2k"};
        const char *air = air_tab[sped & 7];
        p += snprintf(b + p, sizeof(b) - p,
            "\"ok\":true,\"bytes\":%d,\"addr\":\"%02X%02X\",\"sped\":\"0x%02X\","
            "\"parity\":\"%s\",\"baud\":%d,\"air_rate\":\"%s\",\"chan\":%d,"
            "\"freq_mhz\":%d,\"fixed_tx\":%s,\"fec\":%s,\"power_dbm\":%d}",
            n, rsp[1], rsp[2], sped, parity, baud, air, chan,
            410 + chan, (opt & 0x80) ? "true" : "false", (opt & 0x20) ? "true" : "false",
            ((opt & 3) == 0) ? 20 : ((opt & 3) == 1) ? 17 : ((opt & 3) == 2) ? 14 : 10);
    } else if (n > 0) {
        p += snprintf(b + p, sizeof(b) - p, "\"ok\":false,\"bytes\":%d,\"raw\":\"", n);
        for (int i = 0; i < n && p < (int)sizeof(b) - 8; i++)
            p += snprintf(b + p, sizeof(b) - p, "%02X", rsp[i]);
        p += snprintf(b + p, sizeof(b) - p, "\"}");
    } else {
        p += snprintf(b + p, sizeof(b) - p,
            "\"ok\":false,\"bytes\":0,\"hint\":\"rx_idle=0 means module TXD not reaching GPIO14; "
            "check 4 wires + common GND, module VCC, module not T20S variant\"}");
    }
    p += snprintf(b + p, sizeof(b) - p, "}");
    return send_json(req, b);
}

/* ---- /api/diagnostics ---- */
static esp_err_t h_diag(httpd_req_t *req)
{
    CFG_LOCK();
    system_snapshot_t snap; control_get_snapshot(&snap);
    int healthy = sensor_manager_healthy_count();
    storage_flash_wear_t fw; storage_get_flash_wear(&fw);
    he_config_unlock();
    char b[640];
    snprintf(b, sizeof(b),
        "{\"heap_free\":%u,\"heap_min\":%u,\"storage\":%s,\"net\":%s,"
        "\"fault\":\"%s\",\"fault_text\":\"%s\",\"healthy_sensors\":%d,"
        "\"uptime\":%u,\"learned_rate\":%.3f,"
        "\"flash_wear_pct\":%.3f,\"flash_erase_cycles\":%lu,"
        "\"flash_nvs_writes\":%lu,\"flash_fs_kb\":%lu}",
        (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
        storage_healthy() ? "true" : "false", network_is_up() ? "true" : "false",
        fname(snap.active_fault), fault_manager_text(),
        healthy, (unsigned)snap.uptime_ms, fault_manager_learned_rate(),
        (double)fw.est_erase_pct, (unsigned long)fw.est_erase_cycles,
        (unsigned long)fw.nvs_commits, (unsigned long)fw.fs_kb_written);
    return send_json(req, b);
}

/* ---- OTA firmware upload (POST /api/ota) ----
 * Receives a raw firmware image (the .bin produced by `idf.py build`) and
 * streams it to the inactive OTA slot via esp_ota_*. On success, sets the
 * new boot partition and replies 200. The device reboots on the next
 * graceful opportunity (the HTTP response is sent first). */
static esp_err_t h_ota(httpd_req_t *req)
{
    char total_str[16] = {0};
    int total = -1;
    if (httpd_req_get_hdr_value_str(req, "Content-Length", total_str, sizeof(total_str)) == ESP_OK) {
        total = atoi(total_str);
    }
    if (total <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing Content-Length");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "OTA start: %d bytes", total);

    const esp_partition_t *pt = esp_ota_get_next_update_partition(NULL);
    if (!pt) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_OK;
    }
    esp_ota_handle_t oh = 0;
    esp_err_t er = esp_ota_begin(pt, OTA_WITH_SEQUENTIAL_WRITES, &oh);
    if (er != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin failed: %s (target %s, size %ld, free heap %u)",
                 esp_err_to_name(er), pt->label, (long)pt->size,
                 (unsigned)esp_get_free_heap_size());
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_begin failed");
        return ESP_OK;
    }

    uint8_t buf[4096];
    int received = 0;
    while (received < total) {
        int want = (total - received < (int)sizeof(buf)) ? (total - received) : (int)sizeof(buf);
        int got = httpd_req_recv(req, (char *)buf, want);
        if (got <= 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
            esp_ota_abort(oh);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "receive error");
            return ESP_OK;
        }
        er = esp_ota_write(oh, buf, got);
        if (er != ESP_OK) {
            esp_ota_abort(oh);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_write failed");
            return ESP_OK;
        }
        received += got;
        if ((received % (64 * 1024)) == 0)
            ESP_LOGI(TAG, "OTA progress: %d/%d", received, total);
    }

    er = esp_ota_end(oh);
    if (er != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_end failed");
        return ESP_OK;
    }
    er = esp_ota_set_boot_partition(pt);
    if (er != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set_boot failed");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "OTA done: %d bytes -> %s", received, pt->label);
    send_json(req, "{\"ok\":true,\"msg\":\"firmware updated, rebooting\"}");

    /* Defer reboot so the HTTP response is flushed first. */
    xTaskCreate((void (*)(void *))esp_restart, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* ---- registration ---- */
/* Protected API handlers get auth_guard() as the registered handler with the
 * real one in user_ctx. /api/login, /api/logout and the SPA assets stay open
 * (the SPA itself contains no data; data arrives only via authenticated API). */
static httpd_uri_t regs[] = {
    { .uri = "/",            .method = HTTP_GET,  .handler = h_index,        .user_ctx = NULL },
    { .uri = "/style.css",   .method = HTTP_GET,  .handler = h_css,          .user_ctx = NULL },
    { .uri = "/app.js",      .method = HTTP_GET,  .handler = h_js,           .user_ctx = NULL },
    { .uri = "/api/login",   .method = HTTP_POST, .handler = h_login,        .user_ctx = NULL },
    { .uri = "/api/logout",  .method = HTTP_POST, .handler = h_logout,       .user_ctx = NULL },
    { .uri = "/api/password", .method = HTTP_POST, .handler = h_password,    .user_ctx = NULL },
    { .uri = "/api/state",   .method = HTTP_GET,  .handler = h_state,        .user_ctx = NULL },
    { .uri = "/api/profile", .method = HTTP_GET,  .handler = h_profile_get,  .user_ctx = NULL },
    { .uri = "/api/profile", .method = HTTP_POST, .handler = h_profile_post, .user_ctx = NULL },
    { .uri = "/api/profile/file", .method = HTTP_POST, .handler = h_profile_file, .user_ctx = NULL },
    { .uri = "/api/sensor",  .method = HTTP_POST, .handler = h_sensor_post,  .user_ctx = NULL },
    { .uri = "/api/sensor/restore", .method = HTTP_POST, .handler = h_sensor_restore, .user_ctx = NULL },
    { .uri = "/api/sensors/count", .method = HTTP_POST, .handler = h_sensors_count, .user_ctx = NULL },
    { .uri = "/api/boost",   .method = HTTP_POST, .handler = h_boost,        .user_ctx = NULL },
    { .uri = "/api/heating", .method = HTTP_POST, .handler = h_heating,      .user_ctx = NULL },
    { .uri = "/api/device",  .method = HTTP_POST, .handler = h_device,       .user_ctx = NULL },
    { .uri = "/api/pump",    .method = HTTP_POST, .handler = h_pump,         .user_ctx = NULL },
    { .uri = "/api/emergency", .method = HTTP_POST, .handler = h_emergency,  .user_ctx = NULL },
    { .uri = "/api/notify",  .method = HTTP_POST, .handler = h_notify,       .user_ctx = NULL },
    { .uri = "/api/notify/events", .method = HTTP_POST, .handler = h_notify_events, .user_ctx = NULL },
    { .uri = "/api/notify/test", .method = HTTP_POST, .handler = h_notify_test, .user_ctx = NULL },
    { .uri = "/api/network", .method = HTTP_POST, .handler = h_network,      .user_ctx = NULL },
    { .uri = "/api/sim/sensor",  .method = HTTP_POST, .handler = h_sim_sensor,  .user_ctx = NULL },
    { .uri = "/api/sim/heating", .method = HTTP_POST, .handler = h_sim_heating, .user_ctx = NULL },
    { .uri = "/api/fault/clear", .method = HTTP_POST, .handler = h_fault_clear, .user_ctx = NULL },
    { .uri = "/api/limits", .method = HTTP_POST, .handler = h_limits,          .user_ctx = NULL },
    { .uri = "/api/samples", .method = HTTP_GET,  .handler = h_samples,      .user_ctx = NULL },
    { .uri = "/api/daily",   .method = HTTP_GET,  .handler = h_daily,        .user_ctx = NULL },
    { .uri = "/api/log",     .method = HTTP_GET,  .handler = h_log,          .user_ctx = NULL },
    { .uri = "/api/log/clear", .method = HTTP_POST, .handler = h_log_clear,  .user_ctx = NULL },
    { .uri = "/api/diagnostics", .method = HTTP_GET,  .handler = h_diag,      .user_ctx = NULL },
    { .uri = "/api/lora/test",   .method = HTTP_GET,  .handler = h_lora_test, .user_ctx = NULL },
    { .uri = "/api/ota",       .method = HTTP_POST, .handler = h_ota,         .user_ctx = NULL },
};

/* Wrap each protected handler: registered .handler = auth_guard, real handler
 * moved to .user_ctx (index-identical mapping, so the loop below stays simple). */
static void regs_install_guards(void)
{
    size_t n = sizeof(regs) / sizeof(regs[0]);
    for (size_t i = 0; i < n; i++) {
        const char *u = regs[i].uri;
        /* Open resources: SPA assets (no data inside) + login/logout. */
        if (strcmp(u, "/") == 0 || strcmp(u, "/style.css") == 0 || strcmp(u, "/app.js") == 0) continue;
        if (strcmp(u, "/api/login") == 0 || strcmp(u, "/api/logout") == 0) continue;
        regs[i].user_ctx = (void *)regs[i].handler;                   /* real handler */
        regs[i].handler  = auth_guard;                                /* trampoline  */
    }
}

void web_ui_init(system_config_t *cfg)
{
    s_cfg = cfg;
    if (!s_sess_mutex) s_sess_mutex = xSemaphoreCreateMutex();
    if (!s_hist_mutex) s_hist_mutex = xSemaphoreCreateMutex();
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = sizeof(regs) / sizeof(regs[0]);
    hc.lru_purge_enable = true;
    hc.stack_size = 8192;
    regs_install_guards();
    if (httpd_start(&s_srv, &hc) == ESP_OK) {
        unsigned registered = 0;
        for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
            if (httpd_register_uri_handler(s_srv, &regs[i]) == ESP_OK) registered++;
            else ESP_LOGE(TAG, "handler register FAILED: %s (method %d)", regs[i].uri, (int)regs[i].method);
        }
        ESP_LOGI(TAG, "HTTP server started, %u/%u handlers",
                 registered, (unsigned)(sizeof(regs) / sizeof(regs[0])));
    } else {
        ESP_LOGE(TAG, "failed to start HTTP server");
    }
}