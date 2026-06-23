#pragma once

#include <stdint.h>
#include <stddef.h>

#define PRNG_BLOCK_BYTES 64

typedef struct {
    uint8_t  key[32];
    uint8_t  nonce[8];
    uint64_t block_counter;
    uint8_t  block[PRNG_BLOCK_BYTES];
    int      block_pos;
    int      bit_pos;
} MeteorPRNG;

/*
 * Derive key from arbitrary input material and initialise PRNG.
 * input / input_len : any caller-supplied key material (passphrase, shared secret, …)
 * salt / salt_len   : random 32-byte value shared between encoder and decoder
 *                     out-of-band; may be NULL for testing (uses zero salt).
 * Returns 0 on success, -1 on libsodium error.
 */
int prng_init(MeteorPRNG*    prng,
              const uint8_t* input,     size_t input_len,
              const uint8_t* salt,      size_t salt_len);

/*
 * Initialise PRNG directly from a pre-derived 32-byte key.
 * Returns 0 on success, -1 on error.
 */
int prng_init_raw(MeteorPRNG* prng, const uint8_t key[32]);

/* Return next n bits (1 ≤ n ≤ 31) as uint32_t, MSB-first. */
uint32_t prng_next_bits(MeteorPRNG* prng, int n);

/* Securely wipe key material from memory. */
void prng_wipe(MeteorPRNG* prng);
