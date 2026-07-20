/*
    Copyright 2016 Benjamin Vedder benjamin@vedder.se
    Adapted from VESC firmware (GPL-3.0).
*/

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

unsigned short crc16(const unsigned char *buf, unsigned int len);
unsigned short crc16_with_init(const unsigned char *buf, unsigned int len,
                               unsigned short cksum);
uint32_t       crc32_with_init(const uint8_t *buf, uint32_t len, uint32_t cksum);

/* CRC-32C (Castagnoli) — matches VESC Tool's Utility::crc32c, used for the
 * config signature. Use the streaming trio to hash many fragments, or the
 * one-shot crc32c() for a contiguous buffer. */
uint32_t       crc32c_init(void);
uint32_t       crc32c_update(uint32_t crc, const uint8_t *buf, uint32_t len);
uint32_t       crc32c_final(uint32_t crc);
uint32_t       crc32c(const uint8_t *buf, uint32_t len);

#ifdef __cplusplus
}
#endif
