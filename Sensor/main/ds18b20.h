/**
 * DS18B20 temperature driver (ESP-IDF).
 *
 * Built on the onewire bit-banging layer. Issues a CONVERT_T on all
 * connected probes (SKIP_ROM), waits for conversion, then reads each
 * scratchpad to get the 12-bit temperature in centi-degrees Celsius.
 *
 * The conversion time depends on the configured resolution; we always
 * use the power-on default 12-bit (750 ms worst case) and wait for the
 * bus to go high (conversion complete) with a bounded timeout.
 */
#pragma once

#include "onewire.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A single DS18B20 reading. Temperature is in centi-degrees Celsius
 * (e.g. 2150 == 21.50 C). centi == INT16_MIN signals a read failure. */
typedef struct {
    uint8_t id;            /* 1-based index assigned at discovery */
    ow_rom_t rom;          /* ROM address                         */
    int16_t centi;         /* temperature * 100                   */
} ds18b20_reading_t;

/* Discover all DS18B20 devices on the bus (up to max). Returns count. */
int ds18b20_enumerate(ds18b20_reading_t *out, int max);

/* Trigger a conversion on ALL devices (SKIP_ROM + CONVERT_T). */
bool ds18b20_request_all(void);

/* Read the temperature of a specific ROM (centi-degrees, INT16_MIN on error). */
int16_t ds18b20_read(const ow_rom_t *rom);

/* Convenience: request + read all enumerated devices in one call. */
int ds18b20_read_all(ds18b20_reading_t *out, int count);

#ifdef __cplusplus
}
#endif