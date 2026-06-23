#pragma once

#include "prng.h"
#include "bits.h"
#include <stdint.h>

/* Internal end-of-word marker — never written to output covertext. */
#define EOW_TOKEN "\x01"

/* One entry in a syllable distribution. */
typedef struct {
    char    text[64];
    float   p;
    int     slot_start;
    int     slot_end;
    int     slot_count;
} MeteorSlot;

typedef struct {
    MeteorSlot* slots;
    int         count;
    int         beta;
    int         total_slots; /* 2^beta */
} MeteorDist;

/* Result of one encode/decode step. */
typedef struct {
    char     chosen[64];
    uint32_t mask_bits;
    uint32_t r;
    int      cp_len;           /* common prefix length (bits recovered) */
    uint8_t  recovered[32];   /* recovered bits, length = cp_len */
} MeteorStepResult;

/*
 * Build slot table from a probability distribution.
 * syllables[i] / probs[i]: syllable string and its probability (must be > 0).
 * beta: bits per step.
 * Normalises probabilities and assigns integer slot ranges in [0, 2^beta).
 * Caller frees with meteor_free_dist().
 */
MeteorDist* meteor_build_dist(const char** syllables, const float* probs,
                               int count, int beta);
void        meteor_free_dist(MeteorDist* dist);

/*
 * Encode one step: consumes up to beta bits from msg_bits[bit_offset..].
 * Advances prng state by beta bits.
 */
MeteorStepResult meteor_encode_step(const MeteorDist* dist,
                                    const uint8_t* msg_bits, size_t bit_offset,
                                    size_t total_bits,
                                    MeteorPRNG* prng, int beta);

/*
 * Decode one step: given the chosen syllable and distribution, recover bits.
 * Advances prng state by beta bits.
 * Writes recovered bits (0/1 per byte) to out_bits[0..cp_len).
 * Returns number of bits recovered.
 */
int meteor_decode_step(const char*       chosen_syllable,
                       const MeteorDist* dist,
                       MeteorPRNG*       prng,
                       int               beta,
                       uint8_t*          out_bits);
