#include "lora_receiver.h"
#include "app_config.h"
#include "sensor_manager.h"
#include "data_model.h"
#include "storage_manager.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "lora";

/* ---- Binary protocol constants (LoRa.txt, Arduino node) ----
 * Frame: <0x02 0xE3><payload>T<id># — magic is two RAW bytes and the frame
 * terminator is '#'. The controller id is 9 for control frames it sends. */
#define HE_LORA_BIN_MAGIC0   0x02
#define HE_LORA_BIN_MAGIC1   0xE3
#define HE_LORA_BIN_CTRL_ID  9

/* Set while the web /api/lora/test handler owns the UART: the rx task then
 * idles instead of competing for incoming bytes (it would otherwise swallow
 * the module's config response). */
static volatile bool s_test_active = false;

/* ---- Pairing scanner ----
 * Nodes announce themselves with "TT.TTT T<n>&" where n = 0 (unpaired)
 * or n = radio id (NVS-persisted). Every announcement is stored
 * indexed by radio id so the pairing UI can show ALL detected devices
 * with their LoRa ids (not only the latest "T00"). */
#define HE_PAIR_REQ_TTL_MS  30000   /* announcement older than this is stale */

static lora_node_desc_t s_pair_nodes[HE_MAX_SENSORS + 1];  /* [id] */

static void pair_broadcast(const char *fmt, int a, int b);

void lora_pair_request(lora_node_desc_t *nodes, int *count, int max)
{
    if (max > HE_MAX_SENSORS + 1) max = HE_MAX_SENSORS + 1;
    int n = 0;
    for (int i = 0; i <= HE_MAX_SENSORS && n < max; i++) {
        if (s_pair_nodes[i].age_s >= 0) {
            nodes[n++] = s_pair_nodes[i];
        }
    }
    *count = n;
}

/* Broadcast the binary pairing command <0x02 0xE3>PR<id>T9# (LoRa.txt).
 * A node holding factory id 0 stores <id> on receipt; no other pairing
 * flavour is sent. */
void lora_pair_assign(int id)            { pair_broadcast("PR %d", id, id); }

/* Manually send one raw frame (UI debug form). The frame is broadcast as
 * typed (terminator '&' is appended when missing) and repeated a few times
 * so it lands inside a node's RX window. Logged to the event log. */
void lora_raw_send(const char *frame)
{
    char cmd[HE_LORA_LINE_MAX + 4];
    snprintf(cmd, sizeof(cmd), "%s%s", frame,
             strchr(frame, '&') ? "" : "&");
    char ev[HE_LORA_LINE_MAX + 16];
    snprintf(ev, sizeof(ev), "lora tx: %s", cmd);
    storage_log_event(FAULT_NONE, 0, ev);
    for (int i = 0; i < 6; i++) {
        ESP_LOGI(TAG, "tx frame: \"%s\" (%d/6)", cmd, i + 1);
        uart_write_bytes(HE_LORA_UART, (const uint8_t *)cmd, strlen(cmd));
        uart_write_bytes(HE_LORA_UART, "\r\n", 2);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

/* ---- Pairing/unpair verification (async) ----
 * After a PAIR/RESET broadcast arm a verification window (~3 node cycles).
 * The scanner watches the addressed node's fresh announcements:
 *   - pair:   expect "T<rid>"  (node accepted the assignment)
 *   - unpair: expect "T00"     (node back to factory default)
 * Results surface through lora_unpair_verify_state(). */
#define HE_UNPAIR_VERIFY_CYCLES 3

typedef enum {
    UNPAIR_IDLE = 0,
    UNPAIR_PENDING,     /* broadcast done, waiting for the node's answer */
    UNPAIR_CONFIRMED,   /* expected announcement seen                    */
    UNPAIR_FAILED,      /* window elapsed, expected announcement absent  */
} unpair_state_t;

typedef struct {
    unpair_state_t state;
    int      expect_id;       /* radio id expected after the command (0 for unpair) */
    int64_t  deadline_us;     /* verification window end */
} unpair_verify_t;

static unpair_verify_t s_unpair = { .state = UNPAIR_IDLE };

/* Called from process_line() on every fresh announcement. */
static void unpair_verify_note(int id)
{
    if (s_unpair.state != UNPAIR_PENDING) return;
    if (esp_timer_get_time() > s_unpair.deadline_us) {
        s_unpair.state = UNPAIR_FAILED;
        return;
    }
    if (id == s_unpair.expect_id)
        s_unpair.state = UNPAIR_CONFIRMED;
}

/* Arm the verification window. expect_id = the radio id the node should
 * announce after the broadcast (0 = unpaired/reset case). */
void lora_unpair_verify_start(int radio_id, int window_s)
{
    s_unpair.state       = UNPAIR_PENDING;
    s_unpair.expect_id   = radio_id;
    s_unpair.deadline_us = esp_timer_get_time() + (int64_t)window_s * 1000000;
}

/* Poll verification progress. Returns: 0 idle, 1 pending, 2 confirmed,
 * 3 failed. Called by the web layer to report the outcome and offer a
 * force-delete fallback. */
int lora_unpair_verify_state(void)
{
    if (s_unpair.state == UNPAIR_PENDING &&
        esp_timer_get_time() > s_unpair.deadline_us)
        s_unpair.state = UNPAIR_FAILED;
    return (int)s_unpair.state;
}
void lora_unpair_verify_clear(void) { s_unpair.state = UNPAIR_IDLE; }

static void pair_broadcast(const char *fmt, int a, int b)
{
    (void)fmt; (void)b;
    /* Binary pairing command for the Arduino node (LoRa.txt):
     * <0x02 0xE3>PR<id>T9#. A node holding factory id 0 stores <id> on
     * receipt. This is the only pairing frame the controller sends. */
    uint8_t bincmd[16];
    size_t binlen = 0;
    bincmd[binlen++] = HE_LORA_BIN_MAGIC0;
    bincmd[binlen++] = HE_LORA_BIN_MAGIC1;
    binlen += (size_t)snprintf((char *)bincmd + binlen, sizeof(bincmd) - binlen - 1,
                               "PR%dT%d#", a, HE_LORA_BIN_CTRL_ID);

    char ev[64];
    snprintf(ev, sizeof(ev), "lora tx: PR%dT%d# (bin)", a, HE_LORA_BIN_CTRL_ID);
    storage_log_event(FAULT_NONE, 0, ev);
    /* The node listens for pairing commands only inside its RX window once
     * per cycle. Broadcast long enough to cover one full node cycle so the
     * command is guaranteed to land inside a window. */
    for (int i = 0; i < 60; i++) {
        ESP_LOGI(TAG, "tx frame (bin): PR%dT%d# (%d/60)", a, HE_LORA_BIN_CTRL_ID, i + 1);
        uart_write_bytes(HE_LORA_UART, bincmd, binlen);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    ESP_LOGI(TAG, "pairing broadcast sent (bin): PR%dT%d#", a, HE_LORA_BIN_CTRL_ID);
    /* The request is consumed: UI should not offer a stale announcement. */
}

void lora_receiver_test_mode(bool on)
{
    s_test_active = on;
}

/* Parse one line "TT.TTT T<id>&" into (id, temperature).
 * Returns true on success. id is 1..HE_MAX_SENSORS; temperature in degC.
 * Also accepts "ERR T&" as a sensor-failure marker (id=0, temp=NAN). */
static bool parse_line(const char *line, int *out_id, float *out_temp)
{
    /* Minimal validation: must contain '&' and 'T'. */
    const char *amp = strchr(line, '&');
    if (!amp) return false;
    const char *t = strchr(line, 'T');
    if (!t) return false;

    /* "ERR T&" → failure marker. */
    if (strncmp(line, "ERR", 3) == 0) {
        *out_id = 0;
        *out_temp = he_nan();
        return true;
    }

    /* Temperature is the float before the first space. */
    char *endp = NULL;
    float temp = strtof(line, &endp);
    if (endp == line || endp >= amp) return false;

    /* Skip spaces, expect 'T', then the numeric id. */
    while (*endp == ' ') endp++;
    if (*endp != 'T') return false;
    endp++;
    long id = strtol(endp, &endp, 10);
    if (id < 0 || id > HE_MAX_SENSORS) return false;
    /* The id may be followed by '&' or whitespace+&'. */
    while (*endp == ' ' || *endp == '\r' || *endp == '\n') endp++;
    if (*endp != '&') return false;

    *out_id = (int)id;
    *out_temp = temp;
    return true;
}

/* ---- Binary protocol (LoRa.txt, Arduino node) ----
 * The node sends measurements (payload = temperature as "%02.3f") or "ERR"
 * for a probe failure; the controller answers with an "X<id>" ack payload and
 * pairs with a "PR<id>" payload, both carrying the controller id 9. */

/* Log a binary frame in a printable form (raw 0x02/0xE3 would corrupt the
 * event log viewer). */
static void bin_log_rx(const uint8_t *f, size_t len)
{
    char ev[HE_LORA_LINE_MAX * 2];
    int p = snprintf(ev, sizeof(ev), "lora rx: ");
    for (size_t i = 0; i < len && p < (int)sizeof(ev) - 5; i++) {
        uint8_t c = f[i];
        if (c >= 0x20 && c < 0x7f) ev[p++] = (char)c;
        else p += snprintf(ev + p, sizeof(ev) - (size_t)p, "<%02X>", c);
    }
    ev[p] = '\0';
    storage_log_event(FAULT_NONE, 0, ev);
}

/* Parse "<0x02 0xE3><payload>T<id>#". Returns 0 when the bytes are not a
 * binary frame, 1 for a measurement / "ERR" payload, 2 for a control frame
 * (the node's own "X<n>" ack or "PR<n>" pairing reply — nothing for the
 * controller to do with those). "ERR" payload => temp = NaN. */
static int parse_binary(const uint8_t *f, size_t len, int *out_id, float *out_temp)
{
    if (len < 6 || len > HE_LORA_LINE_MAX) return 0;
    if (f[0] != HE_LORA_BIN_MAGIC0 || f[1] != HE_LORA_BIN_MAGIC1) return 0;
    if (f[len - 1] != '#') return 0;

    const uint8_t *t = NULL;
    for (size_t i = len - 2; i >= 2; i--)
        if (f[i] == 'T') { t = f + i; break; }
    if (!t) return 0;

    size_t plen = (size_t)(t - (f + 2));
    if (plen < 1 || plen >= 24) return 0;
    char payload[24];
    memcpy(payload, f + 2, plen);
    payload[plen] = '\0';

    /* Control frames (the node's own "X<n>" ack or "PR<n>" pairing reply)
     * carry the controller id 9, so classify them BEFORE the id range check
     * — nothing for the controller to do with them. */
    if (payload[0] == 'X' || (payload[0] == 'P' && payload[1] == 'R'))
        return 2;

    char idbuf[4];
    size_t idlen = (size_t)(f + len - 1 - (t + 1));
    if (idlen < 1 || idlen >= sizeof(idbuf)) return 0;
    memcpy(idbuf, t + 1, idlen);
    idbuf[idlen] = '\0';
    int id = atoi(idbuf);
    if (id < 0 || id > HE_MAX_SENSORS) return 0;

    if (strcmp(payload, "ERR") == 0) {
        *out_id = id;
        *out_temp = he_nan();
        return 1;
    }
    char *endp = NULL;
    float temp = strtof(payload, &endp);
    if (endp == payload || *endp != '\0') return 0;  /* not a measurement */
    *out_id = id;
    *out_temp = temp;
    return 1;
}

/* Controller -> node ack per LoRa.txt: <0x02 0xE3>X<id>T9#. Sent three
 * times with a gap: the node's half-duplex module needs a moment to switch
 * from TX back to RX, and its SoftwareSerial buffer may still hold the boot
 * banner on the first attempt — a repeat guarantees a clean copy lands. */
static void send_binary_ack(int id)
{
    uint8_t ack[12];
    size_t n = 0;
    ack[n++] = HE_LORA_BIN_MAGIC0;
    ack[n++] = HE_LORA_BIN_MAGIC1;
    n += (size_t)snprintf((char *)ack + n, sizeof(ack) - n - 1,
                          "X%dT%d#", id, HE_LORA_BIN_CTRL_ID);
    for (int i = 0; i < 3; i++) {
        vTaskDelay(pdMS_TO_TICKS(60));
        uart_write_bytes(HE_LORA_UART, ack, n);
    }
    char ev[32];
    snprintf(ev, sizeof(ev), "lora tx: X%dT%d# (bin x3)", id, HE_LORA_BIN_CTRL_ID);
    storage_log_event(FAULT_NONE, 0, ev);
    ESP_LOGI(TAG, "tx ack (bin x3): X%dT%d#", id, HE_LORA_BIN_CTRL_ID);
}

/* Handle a validated binary frame: pairing announcements (id 0), probe
 * failures (ERR), and measurements. Mirrors the text path's bookkeeping so
 * the pairing UI and sensor manager work identically for both protocols. */
static void process_binary(int id, float temp, const uint8_t *raw, size_t len)
{
    bin_log_rx(raw, len);

    if (he_isnan(temp)) {
        ESP_LOGW(TAG, "sensor T%d reports ERR (bin)", id);
        send_binary_ack(id);
        return;
    }
    if (id == 0) {
        /* Unpaired node announcement (LoRa.txt: factory sensor id is 0). */
        s_pair_nodes[0].id = 0;
        s_pair_nodes[0].temp = temp;
        s_pair_nodes[0].age_s = 0;
        unpair_verify_note(0);
        ESP_LOGI(TAG, "pairing request (bin): T0 = %.2f C", temp);
        send_binary_ack(id);
        return;
    }
    if (temp < HE_TEMP_MIN_LOGICAL || temp > HE_TEMP_MAX_LOGICAL) {
        ESP_LOGW(TAG, "T%d out of range: %.2f", id, temp);
        return;
    }
    s_pair_nodes[id].id = id;
    s_pair_nodes[id].temp = temp;
    s_pair_nodes[id].age_s = 0;
    unpair_verify_note(id);

    he_config_lock();
    sensor_manager_lora_update(id, temp);
    he_config_unlock();
    ESP_LOGI(TAG, "T%d = %.2f C (bin)", id, temp);
    send_binary_ack(id);
}

static void process_line(const char *line)
{
    /* Binary frame? Resync on the raw magic bytes at any offset so leading
     * garbage from a previous frame cannot hide a valid one. */
    size_t len = strlen(line);
    for (size_t i = 0; i + 1 < len; i++) {
        if ((uint8_t)line[i] == HE_LORA_BIN_MAGIC0 &&
            (uint8_t)line[i + 1] == HE_LORA_BIN_MAGIC1) {
            int bid;
            float btemp;
            int kind = parse_binary((const uint8_t *)line + i, len - i, &bid, &btemp);
            if (kind == 1) {
                process_binary(bid, btemp, (const uint8_t *)line + i, len - i);
                return;
            }
            if (kind == 2) {
                /* Node-side control frame (X/PR): log once and ignore. */
                bin_log_rx((const uint8_t *)line + i, len - i);
                return;
            }
            break;   /* magic-like bytes but malformed: fall through to text */
        }
    }

    char ev[40];
    snprintf(ev, sizeof(ev), "lora rx: %s", line);
    storage_log_event(FAULT_NONE, 0, ev);
    ESP_LOGI(TAG, "rx frame: \"%s\"", line);
    int id;
    float temp;
    if (!parse_line(line, &id, &temp)) {
        ESP_LOGW(TAG, "malformed line: %s", line);
        return;
    }

    if (id == 0) {
        /* "TT.TTT T00&" = unpaired node announcement (distinct from the
         * "ERR T&" failure marker handled above, which arrives with a NaN
         * temperature). Record only real announcements for the web pairing
         * UI; do not feed them to the sensor manager. */
        if (he_isnan(temp)) {
            ESP_LOGW(TAG, "sensor reports error (ERR T&)");
            return;
        }
        /* Record the announcement indexed by radio id (0 = unpaired). */
        s_pair_nodes[0].id = 0;
        s_pair_nodes[0].temp = temp;
        s_pair_nodes[0].age_s = 0;
        unpair_verify_note(0);
        ESP_LOGI(TAG, "pairing request: T00 = %.2f C", temp);
        return;
    }

    if (temp < HE_TEMP_MIN_LOGICAL || temp > HE_TEMP_MAX_LOGICAL) {
        ESP_LOGW(TAG, "T%d out of range: %.2f", id, temp);
        return;
    }

    /* Track paired-node announcements for the pairing scanner too */
    s_pair_nodes[id].id = id;
    s_pair_nodes[id].temp = temp;
    s_pair_nodes[id].age_s = 0;
    unpair_verify_note(id);

    if (temp < HE_TEMP_MIN_LOGICAL || temp > HE_TEMP_MAX_LOGICAL) {
        ESP_LOGW(TAG, "T%d out of range: %.2f", id, temp);
        return;
    }

    /* Forward under the shared config lock — sensor_manager_poll() and the
     * HTTP handlers touch the same sensor_t array from other tasks/cores. */
    he_config_lock();
    sensor_manager_lora_update(id, temp);
    he_config_unlock();

    ESP_LOGI(TAG, "T%d = %.2f C", id, temp);

    /* Acknowledge the addressed node only: "ACK <id>&" — a node ignores
     * ACK frames carrying another node's radio id. */
    char ack[16];
    snprintf(ack, sizeof(ack), "ACK %d&", id);
    uart_write_bytes(HE_LORA_UART, (const uint8_t *)ack, strlen(ack));
    snprintf(ev, sizeof(ev), "lora tx: ACK -> T%d", id);
    storage_log_event(FAULT_NONE, 0, ev);
    ESP_LOGI(TAG, "tx ack: \"%s\"", ack);
}

static void lora_rx_task(void *arg)
{
    char line[HE_LORA_LINE_MAX + 1];
    int  line_len = 0;

    /* Age all tracked announcements every second (seconds granularity). */
    int64_t age_tick_us = esp_timer_get_time();

    for (;;) {
        if (s_test_active) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Age tracked announcements once per second, driven by the wall
         * clock — NOT by frame arrivals. The old code sat behind the
         * `got <= 0` continue below, so with a frame every ~21 s age_s
         * advanced once per frame instead of once per second: a stale
         * announcement (e.g. a pre-pairing T0) lingered for ~30 frames
         * (~10 min) instead of the 30 s TTL, keeping the "unpaired node"
         * banner and the pairing modal alive with no live T0 frames. */
        if (esp_timer_get_time() - age_tick_us >= 1000000) {
            age_tick_us = esp_timer_get_time();
            for (int i = 0; i <= HE_MAX_SENSORS; i++) {
                if (s_pair_nodes[i].age_s >= 0) {
                    if (++s_pair_nodes[i].age_s > HE_PAIR_REQ_TTL_MS / 1000)
                        s_pair_nodes[i].age_s = -1;
                }
            }
        }

        uint8_t b;
        int got = uart_read_bytes(HE_LORA_UART, &b, 1, pdMS_TO_TICKS(100));
        if (got <= 0) continue;

        /* End-of-line delimiters: '\n' terminates a frame, '\r' ignored. */
        if (b == '\r') continue;
        if (b == '\n') {
            if (line_len > 0) {
                line[line_len] = '\0';
                process_line(line);
                line_len = 0;
            }
            continue;
        }
        /* The protocol terminators are '&' (text / ESP-IDF node) and '#'
         * (binary / Arduino node); flush at either so a missing CRLF does
         * not starve the parser. */
        if (line_len < HE_LORA_LINE_MAX) {
            line[line_len++] = (char)b;
        }
        if (b == '&' || b == '#') {
            line[line_len] = '\0';
            process_line(line);
            line_len = 0;
        }
    }
}

/* ---- E32 module register configuration (LoRa.txt: same channel both ends) ----
 * In mode 3 (M0=M1=1) the E32-433T20D answers the three-byte query C1 C1 C1
 * with "C0 ADDH ADDL SPED CHAN OPTION", and saves a "C0 + 5 params" write to
 * flash. The controller only ever changes CHAN (and only when it differs), so
 * SPED/address/option — which the node's module also relies on — stay intact. */
#define HE_E32_MODE_SETTLE_MS  120
#define HE_E32_RSP_TIMEOUT_MS  400

static void e32_set_mode(int mode)
{
    gpio_set_level(HE_GPIO_LORA_AUX,  mode);
    gpio_set_level(HE_GPIO_LORA_TXRX, mode);
    vTaskDelay(pdMS_TO_TICKS(HE_E32_MODE_SETTLE_MS));
}

/* Read the module's saved registers into out[6] (C0 ADDH ADDL SPED CHAN OPT).
 * Caller must already be in config mode. */
static bool e32_read(uint8_t out[6])
{
    uart_flush_input(HE_LORA_UART);
    const uint8_t q[3] = { 0xC1, 0xC1, 0xC1 };
    uart_write_bytes(HE_LORA_UART, q, sizeof(q));
    int n = uart_read_bytes(HE_LORA_UART, out, 6, pdMS_TO_TICKS(HE_E32_RSP_TIMEOUT_MS));
    return n == 6 && out[0] == 0xC0;
}

/* Save parameters (C0 + ADDH ADDL SPED CHAN OPT). Tolerant of a missing
 * echo: the caller re-reads to verify. Caller must be in config mode. */
static bool e32_write(const uint8_t params[6])
{
    uint8_t w[6];
    w[0] = 0xC0;
    memcpy(w + 1, params + 1, 5);
    uart_flush_input(HE_LORA_UART);
    uart_write_bytes(HE_LORA_UART, w, sizeof(w));
    uint8_t echo[6];
    (void)uart_read_bytes(HE_LORA_UART, echo, sizeof(echo), pdMS_TO_TICKS(HE_E32_RSP_TIMEOUT_MS));
    return true;
}

/* Read the module's saved registers (pauses the rx task while it owns the
 * UART). Returns false when the module does not answer. */
bool lora_module_read_params(uint8_t out[6])
{
    lora_receiver_test_mode(true);
    e32_set_mode(HE_LORA_MODE_CONFIG);
    bool ok = e32_read(out);
    e32_set_mode(HE_LORA_MODE_NORMAL);
    lora_receiver_test_mode(false);
    return ok;
}

/* Force the module onto the given RF channel (0..83 => 410..493 MHz) and
 * verify by read-back. Returns false when the module stays silent. */
bool lora_channel_set(int chan)
{
    if (chan < 0 || chan > 83) return false;
    lora_receiver_test_mode(true);
    e32_set_mode(HE_LORA_MODE_CONFIG);

    uint8_t p[6];
    bool ok = e32_read(p);
    if (ok && p[4] != (uint8_t)chan) {
        uint8_t want[6];
        memcpy(want, p, sizeof(want));
        want[4] = (uint8_t)chan;
        e32_write(want);
        vTaskDelay(pdMS_TO_TICKS(100));
        ok = e32_read(p) && p[4] == (uint8_t)chan;
        if (ok)
            ESP_LOGI(TAG, "E32 channel set to %d (%d MHz)", chan, 410 + chan);
    }

    e32_set_mode(HE_LORA_MODE_NORMAL);
    lora_receiver_test_mode(false);
    return ok;
}

void lora_receiver_init(void)
{
    /* Age all tracked announcements on start so none are treated as
     * fresh until the node actually announces itself. */
    for (int i = 0; i <= HE_MAX_SENSORS; i++)
        s_pair_nodes[i].age_s = -1;

    /* Mode-control pins: both LOW for normal Tx/Rx operation. */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << HE_GPIO_LORA_AUX) | (1ULL << HE_GPIO_LORA_TXRX),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(HE_GPIO_LORA_AUX,  HE_LORA_MODE_NORMAL);
    gpio_set_level(HE_GPIO_LORA_TXRX, HE_LORA_MODE_NORMAL);

    const uart_config_t uc = {
        .baud_rate  = HE_LORA_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    ESP_ERROR_CHECK(uart_driver_install(HE_LORA_UART, HE_LORA_BUF_SIZE * 2,
                                         HE_LORA_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(HE_LORA_UART, &uc));
    ESP_ERROR_CHECK(uart_set_pin(HE_LORA_UART, HE_LORA_UART_TX, HE_LORA_UART_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    /* LoRa.txt: the node and the controller must transmit on the same RF
     * channel. Read the module's saved registers and, when a field-provisioned
     * module sits on another channel, rewrite CHAN to HE_LORA_CHANNEL so the
     * link cannot silently break. Runs before the rx task starts, so it owns
     * the UART exclusively. */
    vTaskDelay(pdMS_TO_TICKS(300));
    e32_set_mode(HE_LORA_MODE_CONFIG);
    uint8_t p[6];
    if (e32_read(p)) {
        ESP_LOGI(TAG, "E32: addr=%02X%02X sped=0x%02X chan=%d opt=0x%02X (%d MHz)",
                 p[1], p[2], p[3], p[4], p[5], 410 + p[4]);
        if (p[4] != HE_LORA_CHANNEL) {
            uint8_t want[6];
            memcpy(want, p, sizeof(want));
            want[4] = HE_LORA_CHANNEL;
            e32_write(want);
            vTaskDelay(pdMS_TO_TICKS(100));
            if (e32_read(p) && p[4] == HE_LORA_CHANNEL)
                ESP_LOGI(TAG, "E32 channel synced to %d (%d MHz)",
                         HE_LORA_CHANNEL, 410 + HE_LORA_CHANNEL);
            else
                ESP_LOGW(TAG, "E32 channel sync FAILED (still %d)", p[4]);
        }
    } else {
        ESP_LOGW(TAG, "E32 parameter read failed — channel left as-is");
    }
    e32_set_mode(HE_LORA_MODE_NORMAL);

    xTaskCreate(lora_rx_task, "lora_rx", HE_LORA_RX_TASK_STACK, NULL, 5, NULL);
    ESP_LOGI(TAG, "LoRa receiver on UART%d @ %d baud, chan %d",
             HE_LORA_UART, HE_LORA_BAUD, HE_LORA_CHANNEL);
}