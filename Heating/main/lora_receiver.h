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

/* Latest unpaired-node announcement ("TT.TTT T00&"). temp = announced
 * temperature (degC), age_s = seconds since the announcement (or -1 when
 * there is none / it was consumed). */
void lora_pair_request(float *temp, int *age_s);

/* Broadcast the pairing command "PAIR <id>&" to assign an id to the
 * unpaired node (id 1..HE_MAX_SENSORS). */
void lora_pair_assign(int id);

/* Broadcast "REPAIR <old> <new>&" — change the radio id of a defined
 * sensor (only the node currently holding <old> accepts). */
void lora_repair_assign(int old, int id);

#ifdef __cplusplus
}
#endif