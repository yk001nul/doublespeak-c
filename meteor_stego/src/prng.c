#include "prng.h"

#include <string.h>
#include <sodium.h>

static const uint8_t APP_INFO[] = "meteor-stego-v1";

int prng_init(MeteorPRNG*    prng,
              const uint8_t* input,     size_t input_len,
              const uint8_t* salt,      size_t salt_len)
{
    if (sodium_init() < 0)
        return -1;

    /* HKDF-SHA256: extract PRK from input material + salt */
    static const uint8_t zero_salt[32] = {0};
    const uint8_t* effective_salt     = (salt && salt_len > 0) ? salt     : zero_salt;
    size_t         effective_salt_len  = (salt && salt_len > 0) ? salt_len : sizeof(zero_salt);

    /* Extract */
    uint8_t prk[crypto_auth_hmacsha256_BYTES];
    crypto_auth_hmacsha256_state st;
    crypto_auth_hmacsha256_init  (&st, effective_salt, effective_salt_len);
    crypto_auth_hmacsha256_update(&st, input, input_len);
    crypto_auth_hmacsha256_final (&st, prk);

    /* Expand: OKM = HMAC(PRK, info || 0x01) */
    crypto_auth_hmacsha256_init  (&st, prk, sizeof(prk));
    crypto_auth_hmacsha256_update(&st, APP_INFO, sizeof(APP_INFO) - 1);
    uint8_t counter = 0x01;
    crypto_auth_hmacsha256_update(&st, &counter, 1);
    crypto_auth_hmacsha256_final (&st, prng->key);

    sodium_memzero(prk, sizeof(prk));

    memset(prng->nonce, 0, sizeof(prng->nonce));
    prng->block_counter = 0;
    prng->block_pos     = PRNG_BLOCK_BYTES; /* force refill on first call */
    prng->bit_pos       = 0;
    return 0;
}

int prng_init_raw(MeteorPRNG* prng, const uint8_t key[32])
{
    if (sodium_init() < 0)
        return -1;

    memcpy(prng->key, key, 32);
    memset(prng->nonce, 0, sizeof(prng->nonce));
    prng->block_counter = 0;
    prng->block_pos     = PRNG_BLOCK_BYTES;
    prng->bit_pos       = 0;
    return 0;
}

uint32_t prng_next_bits(MeteorPRNG* prng, int n)
{
    uint32_t result = 0;
    for (int i = 0; i < n; i++) {
        if (prng->block_pos >= PRNG_BLOCK_BYTES) {
            static const uint8_t zeros[PRNG_BLOCK_BYTES] = {0};
            /* IETF ChaCha20 (RFC 8439): 96-bit nonce, 32-bit counter */
            crypto_stream_chacha20_ietf_xor_ic(
                prng->block, zeros, PRNG_BLOCK_BYTES,
                prng->nonce, prng->block_counter++, prng->key);
            prng->block_pos = 0;
            prng->bit_pos   = 0;
        }
        uint8_t byte = prng->block[prng->block_pos];
        result = (result << 1) | ((byte >> (7 - prng->bit_pos)) & 1);
        prng->bit_pos++;
        if (prng->bit_pos >= 8) {
            prng->bit_pos = 0;
            prng->block_pos++;
        }
    }
    return result;
}

void prng_wipe(MeteorPRNG* prng)
{
    sodium_memzero(prng, sizeof(MeteorPRNG));
}
