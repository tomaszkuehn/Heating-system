#include "lora_sec.h"

#include <string.h>

#include "mbedtls/md.h"

/* Fixed pairing key baked into both firmwares. 8 bytes. */
static const unsigned char LORA_SEC_KEY[8] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
};

#define LORA_SEC_TAG_LEN 8   /* bytes of HMAC kept (64-bit tag) */

void lora_sec_pair_tag(const char *msg, char out_tag[17])
{
    unsigned char mac[32];
    /* mbedtls_md_hmac is one-shot; SHA-256 is always compiled into mbedtls. */
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    LORA_SEC_KEY, sizeof(LORA_SEC_KEY),
                    (const unsigned char *)msg, strlen(msg), mac);
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < LORA_SEC_TAG_LEN; i++) {
        out_tag[i * 2]     = hexd[mac[i] >> 4];
        out_tag[i * 2 + 1] = hexd[mac[i] & 0xf];
    }
    out_tag[16] = '\0';
}

bool lora_sec_pair_verify(const char *msg, const char *tag_hex)
{
    if (!msg || !tag_hex || strlen(tag_hex) != 16) return false;
    char want[17];
    lora_sec_pair_tag(msg, want);
    volatile int diff = 0;
    for (int i = 0; i < 16; i++) {
        char a = tag_hex[i], b = want[i];
        if (a >= 'A' && a <= 'F') a += 32;
        if (a != b) diff = 1;
    }
    return diff == 0;
}