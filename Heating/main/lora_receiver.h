/**
 * LoRa receiver (spec section 2 — radio link to the remote sensor node).
 *
 * A UART-attached Ebyte E32 LoRa module receives temperature measurements
 * from the remote DS18x20 sensor node (see Sensor/ in this repository) as
 * text lines: "TT.TTT T<id>&\r\n", one per probe. This module owns the UART
 * driver, a FreeRTOS task that accumulates bytes into lines, decodes them,
 * and forwards each valid (id, temperature) pair to sensor_manager via
 * sensor_manager_lora_update().
 *
 * The radio is configured once at init (AUX/TX-RX pins to normal mode);
 * no AT commands are needed on the controller side, which acts purely as
 * a receiver. The link is one-way (the node does not expect ACKs here).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise the UART and start the receiver task. Safe to call once from
 * app_main after sensor_manager_init(). */
void lora_receiver_init(void);

/* Pause (on=true) / resume (on=false) the rx task so the web test handler
 * can exclusively own the UART during an AT-query exchange. */
void lora_receiver_test_mode(bool on);

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

/* Verification window = 3 × the nominal node frame period (~3 × 6 s). */
#define HE_UNPAIR_VERIFY_CYCLES  3
#define HE_UNPAIR_VERIFY_SEC     (HE_UNPAIR_VERIFY_CYCLES * HE_LORA_FRAME_PERIOD_MS / 1000)

/* Latest announcements indexed by radio id. The UI polls
 * lora_pair_request() to enumerate available devices. */
void lora_pair_request(lora_node_desc_t *nodes, int *count, int max);

/* Broadcast the pairing command "PAIR <id>&" to assign an id to the
 * unpaired node (id 1..HE_MAX_SENSORS). */
void lora_pair_assign(int id);

/* Broadcast "REPAIR <old> <new>&" — change the radio id of a defined
 * sensor (only the node currently holding <old> accepts). */
void lora_repair_assign(int old, int id);

/* Broadcast "RESET&" — tell the node(s) to erase the persisted radio id
 * and reboot unpaired (factory default). Used when deleting a sensor. */
void lora_unpair_reset(void);

/* ---- Unpair verification (async, non-blocking) ----
 * After the RESET& broadcast arm a verification window of
 * HE_UNPAIR_VERIFY_CYCLES node cycles. The scanner watches whether the
 * node comes back announcing "T00" (factory default kept) or keeps its
 * old id (reset failed — the UI then offers a force delete). */
void lora_unpair_verify_start(int radio_id, int window_s);
/* 0 = idle, 1 = pending, 2 = confirmed (T00 seen), 3 = failed. */
int  lora_unpair_verify_state(void);
void lora_unpair_verify_clear(void);

#ifdef __cplusplus
}
#endif