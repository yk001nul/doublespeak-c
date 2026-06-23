#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 * Convert message bytes to bit array (MSB-first per byte).
 * Appends 8 zero bits as null terminator.
 * *out_bit_count includes the terminator bits.
 * Caller frees returned buffer with free().
 */
uint8_t* bits_from_bytes(const uint8_t* msg, size_t msg_len, size_t* out_bit_count);

/*
 * Convert bit array back to bytes, stopping at 8 consecutive zero bits.
 * *out_msg_len is set to the number of bytes recovered (excludes terminator).
 * Caller frees returned buffer with free().
 */
uint8_t* bits_to_bytes(const uint8_t* bits, size_t bit_count, size_t* out_msg_len);

/*
 * Extract n bits starting at bit_offset from a bit array.
 * Bits are stored one-per-byte (0 or 1) in bits[].
 */
uint32_t bits_read(const uint8_t* bits, size_t bit_offset, int n);
