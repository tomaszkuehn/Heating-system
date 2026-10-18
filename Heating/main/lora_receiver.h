/**
 * LoRa receiver (spec section 2 — radio link to the remote sensor node).
 *
 * A UART-attached Ebyte E32 LoRa module receives temperature measurements
 * from remote DS18x20 sensor nodes. TWO frame formats are accepted:
 *   - text (ESP-IDF node): "TT.TTT T<id>&\r\n"
 *   - binary (Arduino node, LoRa.txt): <0x02 0xE3><payload>T<id>#
 *     with payload = temperature "%02.3f", "ERR", or a control frame
 *     ("X<id>" ack / "PR<id>" pairing) addressed to a node.
 * This module owns the UART driver, a FreeRTOS task that accumulates bytes
 * into frames, decodes them, forwards each valid (id, temperature) pair to
 * sensor_manager via sensor_manager_lora_update(), and acknowledges the
 * addressed node (text "ACK <id>&" or binary <0x02 0xE3>X<id>T9#).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise the UART and start the receiver task. Safe to call once from
 * app_main after sensor_manager_init(). Also reads the E32 module registers
 * and syncs the RF channel to HE_LORA_CHANNEL (LoRa.txt: both ends must use
 * the same channel). */
void lora_receiver_init(void);

/* Pause (on=true) / resume (on=false) the rx task so the web test handler
 * can exclusively own the UART during an AT-query exchange. */
void lora_receiver_test_mode(bool on);

/* Read the E32 module's saved registers (C0 ADDH ADDL SPED CHAN OPTION) into
 * out[6]. Returns false when the module does not answer. Exposed for the web
 * diagnostics / channel-sync endpoints. */
bool lora_module_read_params(uint8_t out[6]);

/* Force the E32 module onto RF channel chan (0..83 => 410..493 MHz) and verify
 * by read-back. Returns false when the module stays silent. */
bool lora_channel_set(int chan);

/* One detected LoRa device (unpaired "T00" announcement or a paired
 * node "TT.TTT T<n>&"). The UI shows all of them when pairing so the
 * user can pick the device by its radio id. */
typedef struct {
    int   id;       /* radio id 0..HE_MAX_SENSORS (0 = unpaired) */
    float temp;     /* last announced temperature (degC), NaN when stale */
    int   age_s;    /* seconds since this announcement (-1 = never) */
} lora_node_desc_t;

/* Maximum nodes tracked for the pairing scanner (same as HE_MAX_SENSORS). */
#define HE_PAIR_DESC_MAX  HE_MAX_SENSORS

/* ---- Over-the-air pairing (async state machine) ----
 * lora_pair_start() assigns a radio id to an unpaired node: up to
 * HE_PAIR_ATTEMPTS broadcasts of <0x02 0xE3>PR<id>T9#, each followed by
 * HE_PAIR_ACK_WINDOW_MS of listening. The job succeeds only when the node
 * comes back announcing the new id — the window is deliberately longer
 * than the longest possible node frame gap (LoRa.txt: 20 s + 0-2 s jitter).
 * Without that announcement the broadcast is repeated; after the last
 * failed attempt the job ends as failed and NO sensor slot is created.
 * on_result runs in the worker task context (may take the config lock). */
#define HE_PAIR_ATTEMPTS        3
#define HE_PAIR_ACK_WINDOW_MS   (HE_LORA_FRAME_PERIOD_MS + 9000)
typedef void (*lora_pair_result_fn_t)(int rid, bool ok);

/* Start a pairing job. Returns false when rid is out of range or another
 * job is already in flight. */
bool lora_pair_start(int rid, lora_pair_result_fn_t on_result);
bool lora_pair_busy(void);
/* "idle" | "pending" | "confirmed" | "failed" — polled by the UI watch. */
const char *lora_pair_state(void);
/* Radio id of the current/last pairing job (0 when idle). The UI uses it to
 * restore the "pairing in progress" sensor-list placeholder after a reload. */
int lora_pair_rid(void);

/* Latest announcements indexed by radio id. The UI polls
 * lora_pair_request() to enumerate available devices. */
void lora_pair_request(lora_node_desc_t *nodes, int *count, int max);

/* Manually transmit a raw LoRa frame typed in the UI debug form. The frame
 * is sent as typed ('&' appended when missing), repeated 6× so it lands in
 * a node's RX window, and logged to the event log. */
void lora_raw_send(const char *frame);

/* ---- Radio-frame diagnostics (UI link-loss %) ----
 * Cumulative counters since boot, RAM-only:
 * garbage = lines that parsed as neither a text nor a binary frame
 *   (RF noise, collisions, truncated UART data);
 * rejected = well-formed frames dropped before the sensor manager
 *   (out-of-range temperature). Accepted frames are counted per sensor
 * by sensor_manager_link_stats(); silence (no frame at all) surfaces
 * there as MISS. */
void lora_link_stats(uint32_t *garbage, uint32_t *rejected);

#ifdef __cplusplus
}
#endif