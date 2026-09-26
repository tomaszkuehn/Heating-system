/**
 * LoRa pairing security (shared by controller and sensor node).
 *
 * Pairing frames are authenticated with HMAC-SHA256 under a fixed,
 * compile-time key baked into both firmwares. The tag is truncated to the
 * first 8 bytes (64 bit) and hex-encoded — 16 characters keeps the frame
 * inside the 48-byte radio line limit.
 *
 * Frame: "PAIR <id> <nonce8hex> <tag16hex>&"
 *   msg = "PAIR <id> <nonce8hex>"
 *   tag = hex(HMAC-SHA256(key, msg)[0..7])
 *
 * Replay protection lives on the node side: the nonce ring in NVS (see
 * Sensor main.c). The controller draws a fresh random nonce per broadcast.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compute the pairing tag for msg (16 lowercase hex chars + NUL). */
void lora_sec_pair_tag(const char *msg, char out_tag[17]);

/* Constant-time verify: tag_hex must match hex(HMAC(key,msg)[0..7]),
 * case-insensitive, exactly 16 characters. */
bool lora_sec_pair_verify(const char *msg, const char *tag_hex);

#ifdef __cplusplus
}
#endif