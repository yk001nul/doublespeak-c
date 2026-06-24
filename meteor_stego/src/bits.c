#include "bits.h"

#include <stdlib.h>
#include <string.h>

uint8_t* bits_from_bytes(const uint8_t* msg, size_t msg_len, size_t* out_bit_count)
{
    size_t   total = (msg_len + 1) * 8; /* +1 for null terminator byte */
    uint8_t* bits  = (uint8_t*)calloc(total, 1);
    if (!bits) return NULL;

    for (size_t i = 0; i < msg_len; i++) {
        for (int b = 7; b >= 0; b--)
            bits[i * 8 + (7 - b)] = (msg[i] >> b) & 1;
    }
    /* last 8 entries are already 0 (null terminator) via calloc */

    if (out_bit_count) *out_bit_count = total;
    return bits;
}

uint8_t* bits_to_bytes(const uint8_t* bits, size_t bit_count, size_t* out_msg_len)
{
    if (bit_count == 0) {
        if (out_msg_len) *out_msg_len = 0;
        return (uint8_t*)calloc(1, 1);
    }

    /* max possible bytes */
    size_t   max_bytes = bit_count / 8;
    uint8_t* msg       = (uint8_t*)calloc(max_bytes + 1, 1);
    if (!msg) return NULL;

    size_t byte_i = 0;
    for (size_t i = 0; i + 7 < bit_count; i += 8) {
        uint8_t val = 0;
        for (int b = 0; b < 8; b++)
            val = (uint8_t)((val << 1) | (bits[i + b] & 1));

        if (val == 0) {
            /* null terminator found */
            break;
        }
        msg[byte_i++] = val;
    }

    if (out_msg_len) *out_msg_len = byte_i;
    return msg;
}

uint32_t bits_read(const uint8_t* bits, size_t bit_offset, int n)
{
    uint32_t result = 0;
    for (int i = 0; i < n; i++)
        result = (result << 1) | (bits[bit_offset + i] & 1);
    return result;
}
