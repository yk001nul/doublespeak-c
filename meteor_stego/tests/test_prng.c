/*
 * Tests for the PRNG (ChaCha20 + HKDF-SHA256).
 * Run with: ./test_prng
 */
#include "../src/prng.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <sodium.h>

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { if (cond) { printf("[PASS] %s\n", msg); passed++; } \
         else { printf("[FAIL] %s  (line %d)\n", msg, __LINE__); failed++; } } while(0)

/* RFC 8439 §2.1.1 ChaCha20 known-answer test vector */
static void test_chacha20_vector(void)
{
    uint8_t key[32] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
        0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
        0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f
    };
    uint8_t nonce[8] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x4a};
    uint8_t zeros[64] = {0};
    uint8_t stream[64];

    crypto_stream_chacha20_xor_ic(stream, zeros, 64, nonce, 1, key);

    /* RFC 8439 §2.1.1 expected first 4 bytes with counter=1, nonce=...4a */
    CHECK(stream[0] == 0x22, "ChaCha20 vector byte[0]");
    CHECK(stream[1] == 0x4f, "ChaCha20 vector byte[1]");
    CHECK(stream[2] == 0x51, "ChaCha20 vector byte[2]");
    CHECK(stream[3] == 0xf3, "ChaCha20 vector byte[3]");
}

static void test_hkdf_determinism(void)
{
    const uint8_t input[] = "test-passphrase";
    uint8_t       salt[32] = {0x42};

    MeteorPRNG p1, p2;
    prng_init(&p1, input, sizeof(input) - 1, salt, 32);
    prng_init(&p2, input, sizeof(input) - 1, salt, 32);

    uint32_t a1 = prng_next_bits(&p1, 32);
    uint32_t a2 = prng_next_bits(&p2, 32);
    CHECK(a1 == a2, "HKDF: same passphrase+salt produces same output");

    uint32_t b1 = prng_next_bits(&p1, 32);
    uint32_t b2 = prng_next_bits(&p2, 32);
    CHECK(b1 == b2, "HKDF: consecutive outputs match");

    MeteorPRNG p3;
    const uint8_t other[] = "different-passphrase";
    prng_init(&p3, other, sizeof(other) - 1, salt, 32);
    uint32_t c = prng_next_bits(&p3, 32);
    CHECK(a1 != c, "HKDF: different passphrase produces different output");

    prng_wipe(&p1); prng_wipe(&p2); prng_wipe(&p3);
}

static void test_zero_salt(void)
{
    const uint8_t input[] = "abc";
    MeteorPRNG p1, p2;
    prng_init(&p1, input, 3, NULL, 0);
    prng_init(&p2, input, 3, NULL, 0);
    CHECK(prng_next_bits(&p1, 16) == prng_next_bits(&p2, 16),
          "Zero-salt: deterministic");
    prng_wipe(&p1); prng_wipe(&p2);
}

static void test_block_boundary(void)
{
    const uint8_t input[] = "boundary-test";
    MeteorPRNG prng;
    prng_init(&prng, input, sizeof(input) - 1, NULL, 0);

    /* consume more than one 64-byte block worth of bits */
    uint32_t prev = 0;
    int diffs = 0;
    for (int i = 0; i < 100; i++) {
        uint32_t v = prng_next_bits(&prng, 8);
        if (v != prev) diffs++;
        prev = v;
    }
    CHECK(diffs > 50, "Block boundary: output varies across 100 bytes");
    prng_wipe(&prng);
}

static void test_wipe(void)
{
    MeteorPRNG p;
    const uint8_t input[] = "wipe-me";
    prng_init(&p, input, sizeof(input) - 1, NULL, 0);
    prng_next_bits(&p, 8);
    prng_wipe(&p);

    const uint8_t* raw = (const uint8_t*)&p;
    int all_zero = 1;
    for (size_t i = 0; i < sizeof(MeteorPRNG); i++)
        if (raw[i] != 0) { all_zero = 0; break; }
    CHECK(all_zero, "prng_wipe: struct is zeroed");
}

static void test_msb_first(void)
{
    const uint8_t input[] = "msb";
    MeteorPRNG p;
    prng_init(&p, input, 3, NULL, 0);

    /* read 8 bits one-at-a-time vs one 8-bit read */
    MeteorPRNG p2;
    prng_init(&p2, input, 3, NULL, 0);

    uint32_t combined = 0;
    for (int i = 0; i < 8; i++)
        combined = (combined << 1) | prng_next_bits(&p, 1);

    uint32_t bulk = prng_next_bits(&p2, 8);
    CHECK(combined == bulk, "prng_next_bits: 1-bit calls == 8-bit call");

    prng_wipe(&p); prng_wipe(&p2);
}

int main(void)
{
    if (sodium_init() < 0) {
        fprintf(stderr, "sodium_init() failed\n");
        return 1;
    }

    test_chacha20_vector();
    test_hkdf_determinism();
    test_zero_salt();
    test_block_boundary();
    test_wipe();
    test_msb_first();

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
