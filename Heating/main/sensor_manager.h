/**
 * Sensor manager (spec sections 2, 3, 4, 6.1).
 *
 * Owns the external sensor interface (a separate board reached over UART),
 * parses its frames, validates every reading, maintains the 7-sample moving
 * average and the effective temperature, runs open-window detection and
 * drift/stale checks, and computes the weighted system average.
 *
 * When a sensor is sourced from the simulation manager, the UART is bypassed
 * for that sensor (spec 8.1: switch a single sensor between real & simulated).
 */
#pragma once

#include "data_model.h"
#include "simulation_manager.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bind the live sensor table (owned by the config). */
void sensor_manager_init(void);
void sensor_manager_bind(sensor_t *sensors, int count, sensor_t *external);

/* Poll the interface (or simulation) once; updates readings & quality. */
void sensor_manager_poll(void);

/* Weighted system average over logically-correct, active sensors. */
float sensor_manager_system_temp(void);

/* Outdoor reading (NAN if absent/invalid). */
float sensor_manager_external_temp(void);

/* Number of currently healthy sensors. */
int  sensor_manager_healthy_count(void);

/* Manual restore of a sensor suppressed by open-window detection. */
void sensor_manager_restore(int id);

/* Push a temperature reading received over the LoRa radio link (spec 2).
 * id is 1..HE_MAX_SENSORS; temperature in degC. Called by lora_receiver
 * under he_config_lock(). No-op if the id is not bound or the sensor is
 * disabled/simulated. */
void sensor_manager_lora_update(int id, float temperature);

/* Record a radio-frame arrival for a sensor and update its loss estimate
 * (missed/expected over the sliding window of the last 8 arrival slots).
 * Called by sensor_manager_lora_update. Exposed for tests. */
void sensor_manager_note_rx(sensor_t *s);

/* Record a probe read failure (ERR frame) received over the radio link.
 * id is the radio id (1..HE_MAX_SENSORS); adds an ERR entry to that sensor's
 * measurement buffer and counts the frame for link-loss accounting. */
void sensor_manager_lora_err(int radio_id);

/* ---- Measurement-buffer diagnostics (UI "Bufor" panel) ----
 * A chronological ring of the last HE_MEAS_BUF_LEN measurement events per
 * sensor: a valid reading, an ERR frame from the node, or a MISS (an expected
 * frame that never arrived). Kept in RAM only (never persisted to NVS). */
#define HE_MEAS_BUF_LEN  8
typedef enum {
    HE_MEAS_OK = 0,   /* valid measurement            */
    HE_MEAS_ERR,      /* ERR frame (probe read error) */
    HE_MEAS_MISS,     /* expected frame not received  */
} he_meas_kind_t;

typedef struct {
    uint32_t       ts;      /* unix seconds (uptime s before SNTP is valid) */
    float          value;   /* valid only for HE_MEAS_OK                     */
    he_meas_kind_t kind;
} he_meas_slot_t;

/* Copy the last measurement events (oldest first) for a sensor id
 * (0 = external, 1..HE_MAX_SENSORS = internal logical id).
 * Returns the number of slots written (0 when the id is unknown). */
int sensor_manager_get_buffer(int id, he_meas_slot_t *out, int max);

/* Frame protocol constants (external sensor interface). */
#define HE_FRAME_START  0xAA
#define HE_FRAME_END    0x55
#define HE_CMD_POLL     0x01

#ifdef __cplusplus
}
#endif