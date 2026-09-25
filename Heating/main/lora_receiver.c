#include "lora_receiver.h"
#include "app_config.h"
#include "sensor_manager.h"
#include "data_model.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "lora";

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

/* Broadcast a pairing command. mode PAIR (new node, id 1..6) sends
 * "PAIR <id>&"; mode REPAIR (change id of a defined sensor) sends
 * "REPAIR <old> <new>&" which only the node currently holding <old>
 * accepts. Replies "OK T<n>&" surface as normal frames. */
void lora_pair_assign(int id)            { pair_broadcast("PAIR %d&", id, id); }
void lora_repair_assign(int old, int id) { pair_broadcast("REPAIR %d %d&", old, id); }

/* Broadcast "RESET&" — the addressed (or any listening) node erases its
 * persisted radio id and reboots into the factory-default unpaired mode. */
void lora_unpair_reset(void) { pair_broadcast("RESET&", 0, 0); }

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
    char cmd[24];
    snprintf(cmd, sizeof(cmd), fmt, a, b);
    /* The node listens for pairing/repair commands only inside its ~5 s ACK
     * window once per ~7 s cycle. Broadcast long enough to cover one full
     * node cycle so the command is guaranteed to land inside a window. */
    for (int i = 0; i < 30; i++) {
        uart_write_bytes(HE_LORA_UART, cmd, strlen(cmd));
        uart_write_bytes(HE_LORA_UART, "\r\n", 2);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGI(TAG, "pairing broadcast sent: %s", cmd);
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

static void process_line(const char *line)
{
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

    /* Acknowledge to the remote node so it can manage its power level and
     * watchdog. The node expects the single byte 'X' as confirmation. */
    static const uint8_t ack = 'X';
    uart_write_bytes(HE_LORA_UART, &ack, 1);
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
        /* The frame terminator in the protocol is '&'; flush at '&' too so
         * a missing CRLF does not starve the parser. */
        if (line_len < HE_LORA_LINE_MAX) {
            line[line_len++] = (char)b;
        }
        if (b == '&') {
            line[line_len] = '\0';
            process_line(line);
            line_len = 0;
        }

        /* Age stale announcements once per second. */
        if (esp_timer_get_time() - age_tick_us >= 1000000) {
            age_tick_us = esp_timer_get_time();
            for (int i = 0; i <= HE_MAX_SENSORS; i++) {
                if (s_pair_nodes[i].age_s >= 0) {
                    if (++s_pair_nodes[i].age_s > HE_PAIR_REQ_TTL_MS / 1000)
                        s_pair_nodes[i].age_s = -1;
                }
            }
        }
    }
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

    xTaskCreate(lora_rx_task, "lora_rx", HE_LORA_RX_TASK_STACK, NULL, 5, NULL);
    ESP_LOGI(TAG, "LoRa receiver on UART%d @ %d baud", HE_LORA_UART, HE_LORA_BAUD);
}