/**
 * 1-Wire bus driver for the DS18B20 family, ESP-IDF edition.
 *
 * A minimal bit-banging implementation of the 1-Wire timing protocol for
 * a single bus master, using the open-drain mode of a standard GPIO:
 * the pin is driven high for the strong pull-up after a conversion and
 * between time slots, and released to input (with the internal pull-up
 * disabled) during the read/sample window so a slave can pull it low.
 *
 * Supports:
 *   - RESET/presence sequence
 *   - WRITE 0/1, READ bit/byte
 *   - ROM commands: SKIP_ROM, READ_ROM, MATCH_ROM
 *   - Device discovery via SEARCH_ROM (binary tree walk, last-discrepancy)
 *
 * No dynamic allocation, no FreeRTOS dependencies beyond esp_timer for the
 * microsecond clock. Interrupts are disabled only inside the critical
 * timing windows (<= 70 us), so higher-priority ISRs are not blocked for
 * long periods.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1-Wire ROM commands. */
#define OW_CMD_READ_ROM     0x33
#define OW_CMD_SKIP_ROM     0xCC
#define OW_CMD_MATCH_ROM    0x55
#define OW_CMD_SEARCH_ROM   0xF0

/* DS18B20 function commands. */
#define DS18B20_CMD_CONVERT_T   0x44
#define DS18B20_CMD_READ_SCRATCH 0xBE
#define DS18B20_CMD_WRITE_SCRATCH 0x4E
#define DS18B20_CMD_READ_POWER   0xB4

/* A discovered ROM address (8 bytes: 7 address + 1 CRC). */
typedef struct {
    uint8_t rom[8];
} ow_rom_t;

/* Initialise the bus on the given GPIO (must support open-drain / bidir). */
void ow_init(gpio_num_t pin);

/* Issue a RESET and return true if a presence pulse was detected. */
bool ow_reset(void);

/* Write / read primitives. */
void ow_write_bit(bool bit);
bool ow_read_bit(void);
void ow_write_byte(uint8_t byte);
uint8_t ow_read_byte(void);

/* Convenience ROM-level commands (assume single bus master). */
void ow_skip_rom(void);
void ow_match_rom(const uint8_t rom[8]);

/* Search the bus for up to `max` devices, filling `out`. Returns count.
 * Re-entrant: starts a fresh search each call (no persistent state). */
int ow_search(ow_rom_t *out, int max);

/* Family-code helpers. */
bool ow_is_ds18b20(const ow_rom_t *r);
uint8_t ow_crc8(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif