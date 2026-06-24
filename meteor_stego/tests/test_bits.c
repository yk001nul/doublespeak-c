/*
 * Tests for bit packing / unpacking.
 */
#include "../src/bits.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { if (cond) { printf("[PASS] %s\n", msg); passed++; } \
         else { printf("[FAIL] %s  (line %d)\n", msg, __LINE__); failed++; } } while(0)

static void test_roundtrip_single_byte(void)
{
    for (int v = 0; v <= 255; v++) {
        if (v == 0) continue; /* 0 is the null terminator — skip */
        uint8_t  msg[1] = { (uint8_t)v };
        size_t   bit_count;
        uint8_t* bits = bits_from_bytes(msg, 1, &bit_count);
        CHECK(bits != NULL, "bits_from_bytes: non-NULL");
        CHECK(bit_count == 16, "bits_from_bytes: 1 byte → 16 bits (8 data + 8 terminator)");

        size_t   msg_len;
        uint8_t* back = bits_to_bytes(bits, bit_count, &msg_len);
        CHECK(back != NULL,   "bits_to_bytes: non-NULL");
        CHECK(msg_len == 1,   "bits_to_bytes: recovered 1 byte");
        if (back && msg_len == 1)
            CHECK(back[0] == (uint8_t)v, "bits_to_bytes: value matches");

        free(bits); free(back);
    }
}

static void test_msb_first(void)
{
    uint8_t  msg[1] = { 0xAB }; /* 10101011 */
    size_t   bit_count;
    uint8_t* bits = bits_from_bytes(msg, 1, &bit_count);

    int expected[8] = {1,0,1,0,1,0,1,1};
    int ok = 1;
    for (int i = 0; i < 8; i++)
        if (bits[i] != (uint8_t)expected[i]) { ok = 0; break; }
    CHECK(ok, "MSB-first: 0xAB → 10101011");
    free(bits);
}

static void test_bits_read(void)
{
    uint8_t  msg[2] = {0xAB, 0xCD};
    size_t   bit_count;
    uint8_t* bits = bits_from_bytes(msg, 2, &bit_count);

    uint32_t upper = bits_read(bits, 0, 8);
    CHECK(upper == 0xAB, "bits_read: first 8 bits == 0xAB");

    uint32_t lower = bits_read(bits, 8, 8);
    CHECK(lower == 0xCD, "bits_read: next 8 bits == 0xCD");

    uint32_t cross = bits_read(bits, 4, 8);
    CHECK(cross == ((0xAB << 4 | 0xCD >> 4) & 0xFF), "bits_read: cross-byte 8 bits");

    free(bits);
}

static void test_null_terminator(void)
{
    uint8_t  msg[3] = {0x41, 0x42, 0x43}; /* "ABC" */
    size_t   bit_count;
    uint8_t* bits = bits_from_bytes(msg, 3, &bit_count);

    /* last 8 bits should be zero */
    int ok = 1;
    for (size_t i = bit_count - 8; i < bit_count; i++)
        if (bits[i] != 0) { ok = 0; break; }
    CHECK(ok, "Null terminator: last 8 bits are zero");

    size_t   msg_len;
    uint8_t* back = bits_to_bytes(bits, bit_count, &msg_len);
    CHECK(msg_len == 3, "Null terminator: recovered exactly 3 bytes");
    if (back && msg_len == 3)
        CHECK(memcmp(back, msg, 3) == 0, "Null terminator: content matches");
    free(bits); free(back);
}

static void test_multibyte(void)
{
    const char* text = "Hello World!";
    size_t      tlen = strlen(text);
    size_t      bit_count;
    uint8_t* bits = bits_from_bytes((const uint8_t*)text, tlen, &bit_count);

    size_t   msg_len;
    uint8_t* back = bits_to_bytes(bits, bit_count, &msg_len);

    CHECK(msg_len == tlen, "Multibyte: length matches");
    if (back && msg_len == tlen)
        CHECK(memcmp(back, text, tlen) == 0, "Multibyte: content matches");

    free(bits); free(back);
}

int main(void)
{
    test_roundtrip_single_byte();
    test_msb_first();
    test_bits_read();
    test_null_terminator();
    test_multibyte();

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
