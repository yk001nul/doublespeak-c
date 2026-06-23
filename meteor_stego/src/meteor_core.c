#include "meteor_core.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── helpers ──────────────────────────────────────────────────────────────── */

static int imax(int a, int b) { return a > b ? a : b; }

static int common_prefix_len(int slot_start, int slot_end, int beta)
{
    if (slot_start == slot_end) return beta;
    int cp = 0;
    for (int i = beta - 1; i >= 0; i--) {
        if (((slot_start >> i) & 1) == ((slot_end >> i) & 1))
            cp++;
        else
            break;
    }
    return cp;
}

/* ── distribution ─────────────────────────────────────────────────────────── */

MeteorDist* meteor_build_dist(const char** syllables, const float* probs,
                               int count, int beta)
{
    if (count <= 0 || beta < 1 || beta > 31) return NULL;

    MeteorDist* dist = (MeteorDist*)calloc(1, sizeof(MeteorDist));
    if (!dist) return NULL;

    dist->slots       = (MeteorSlot*)calloc((size_t)count, sizeof(MeteorSlot));
    dist->count       = count;
    dist->beta        = beta;
    dist->total_slots = 1 << beta;

    if (!dist->slots) { free(dist); return NULL; }

    /* normalise probabilities */
    float sum = 0.0f;
    for (int i = 0; i < count; i++) sum += probs[i];
    if (sum <= 0.0f) sum = 1.0f;

    int cursor = 0;
    for (int i = 0; i < count; i++) {
        float p = probs[i] / sum;
        int   n = (int)roundf(p * (float)dist->total_slots);
        n = imax(1, n);

        strncpy(dist->slots[i].text, syllables[i], sizeof(dist->slots[i].text) - 1);
        dist->slots[i].text[sizeof(dist->slots[i].text) - 1] = '\0';
        dist->slots[i].p          = p;
        dist->slots[i].slot_start = cursor;
        dist->slots[i].slot_end   = cursor + n - 1;
        dist->slots[i].slot_count = n;
        cursor += n;
    }

    /* clamp last slot to absorb rounding errors */
    dist->slots[count - 1].slot_end   = dist->total_slots - 1;
    dist->slots[count - 1].slot_count = dist->total_slots - dist->slots[count - 1].slot_start;

    return dist;
}

void meteor_free_dist(MeteorDist* dist)
{
    if (!dist) return;
    free(dist->slots);
    free(dist);
}

/* ── encode step ──────────────────────────────────────────────────────────── */

MeteorStepResult meteor_encode_step(const MeteorDist* dist,
                                    const uint8_t* msg_bits, size_t bit_offset,
                                    size_t total_bits,
                                    MeteorPRNG* prng, int beta)
{
    MeteorStepResult res;
    memset(&res, 0, sizeof(res));

    uint32_t mask = prng_next_bits(prng, beta);
    res.mask_bits = mask;

    /* read up to beta message bits (pad with 0 if past end) */
    uint32_t msg = 0;
    for (int i = 0; i < beta; i++) {
        msg <<= 1;
        if (bit_offset + (size_t)i < total_bits)
            msg |= msg_bits[bit_offset + i] & 1;
    }

    uint32_t r = msg ^ mask;
    res.r = r;

    /* find the slot containing r */
    int chosen_idx = dist->count - 1;
    for (int i = 0; i < dist->count; i++) {
        if (r >= (uint32_t)dist->slots[i].slot_start &&
            r <= (uint32_t)dist->slots[i].slot_end) {
            chosen_idx = i;
            break;
        }
    }

    strncpy(res.chosen, dist->slots[chosen_idx].text, sizeof(res.chosen) - 1);
    res.chosen[sizeof(res.chosen) - 1] = '\0';

    int start = dist->slots[chosen_idx].slot_start;
    int end   = dist->slots[chosen_idx].slot_end;
    res.cp_len = common_prefix_len(start, end, beta);

    /* store recovered bits (same as the common prefix of msg) */
    for (int i = 0; i < res.cp_len; i++)
        res.recovered[i] = (msg >> (beta - 1 - i)) & 1;

    return res;
}

/* ── decode step ──────────────────────────────────────────────────────────── */

int meteor_decode_step(const char*       chosen_syllable,
                       const MeteorDist* dist,
                       MeteorPRNG*       prng,
                       int               beta,
                       uint8_t*          out_bits)
{
    uint32_t mask = prng_next_bits(prng, beta);

    /* find the slot for this syllable */
    int idx = dist->count - 1;
    for (int i = 0; i < dist->count; i++) {
        if (strcmp(dist->slots[i].text, chosen_syllable) == 0) {
            idx = i;
            break;
        }
    }

    int start  = dist->slots[idx].slot_start;
    int end    = dist->slots[idx].slot_end;
    int cp_len = common_prefix_len(start, end, beta);

    /* common prefix of the slot range XOR with mask prefix */
    uint32_t common = (uint32_t)start >> (beta - cp_len);
    uint32_t msg_prefix = common ^ (mask >> (beta - cp_len));

    for (int i = 0; i < cp_len; i++)
        out_bits[i] = (msg_prefix >> (cp_len - 1 - i)) & 1;

    return cp_len;
}
