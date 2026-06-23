/*
 * Tests for meteor_core: slot building, encode/decode steps.
 */
#include "../src/meteor_core.h"
#include "../src/prng.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sodium.h>

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { if (cond) { printf("[PASS] %s\n", msg); passed++; } \
         else { printf("[FAIL] %s  (line %d)\n", msg, __LINE__); failed++; } } while(0)

static void test_slot_coverage(void)
{
    const char* syls[] = {"the", "a", "in", "of", "to"};
    float       probs[] = {0.35f, 0.25f, 0.20f, 0.12f, 0.08f};

    MeteorDist* dist = meteor_build_dist(syls, probs, 5, 3);
    CHECK(dist != NULL, "meteor_build_dist: non-NULL");
    if (!dist) return;

    /* last slot must end at total_slots - 1 */
    int last_end = dist->slots[dist->count - 1].slot_end;
    CHECK(last_end == dist->total_slots - 1, "Slot coverage: last slot ends at 2^beta - 1");

    /* slots must be contiguous and non-overlapping */
    int ok = 1;
    for (int i = 1; i < dist->count; i++) {
        if (dist->slots[i].slot_start != dist->slots[i - 1].slot_end + 1) {
            ok = 0; break;
        }
    }
    CHECK(ok, "Slot coverage: contiguous, non-overlapping");

    /* every slot must have at least 1 entry */
    ok = 1;
    for (int i = 0; i < dist->count; i++)
        if (dist->slots[i].slot_count < 1) { ok = 0; break; }
    CHECK(ok, "Slot coverage: every slot >= 1");

    meteor_free_dist(dist);
}

static void test_encode_decode_roundtrip_mock(void)
{
    /* simple 2-candidate distribution with beta=3 */
    const char* syls[] = {"hel", "lo"};
    float       probs[] = {0.6f, 0.4f};
    int         beta = 3;

    const uint8_t key[32] = {
        1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
        17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32
    };

    /* message bits: 1 0 1 */
    uint8_t msg_bits[3] = {1, 0, 1};
    size_t  total_bits  = 3;

    MeteorPRNG enc_prng, dec_prng;
    prng_init_raw(&enc_prng, key);
    prng_init_raw(&dec_prng, key);

    MeteorDist* dist = meteor_build_dist(syls, probs, 2, beta);
    CHECK(dist != NULL, "Roundtrip: dist non-NULL");
    if (!dist) return;

    MeteorStepResult step = meteor_encode_step(dist, msg_bits, 0, total_bits, &enc_prng, beta);
    CHECK(step.cp_len >= 0, "Roundtrip: encode cp_len >= 0");
    CHECK(strlen(step.chosen) > 0, "Roundtrip: chosen syllable non-empty");

    uint8_t recovered[32];
    int nbits = meteor_decode_step(step.chosen, dist, &dec_prng, beta, recovered);
    CHECK(nbits == step.cp_len, "Roundtrip: cp_len matches between encode and decode");

    int bits_match = 1;
    for (int i = 0; i < nbits; i++)
        if (recovered[i] != step.recovered[i]) { bits_match = 0; break; }
    CHECK(bits_match, "Roundtrip: recovered bits match encode step");

    meteor_free_dist(dist);
    prng_wipe(&enc_prng);
    prng_wipe(&dec_prng);
}

static void test_eow_token(void)
{
    const char* syls[] = {"pre", "fix", EOW_TOKEN};
    float       probs[] = {0.4f, 0.35f, 0.25f};
    MeteorDist* dist = meteor_build_dist(syls, probs, 3, 3);
    CHECK(dist != NULL, "EOW token: dist built");

    if (!dist) return;

    /* check EOW token is present in dist */
    int found = 0;
    for (int i = 0; i < dist->count; i++)
        if (strcmp(dist->slots[i].text, EOW_TOKEN) == 0) { found = 1; break; }
    CHECK(found, "EOW token: present in distribution");

    meteor_free_dist(dist);
}

static void test_single_slot_full_bits(void)
{
    /* A single-candidate distribution should give cp_len == beta */
    const char* syls[] = {"only"};
    float       probs[] = {1.0f};
    int         beta = 4;

    MeteorDist* dist = meteor_build_dist(syls, probs, 1, beta);
    CHECK(dist != NULL, "Single slot: dist built");
    if (!dist) return;

    /* slot covers [0, 2^beta - 1], so cp_len should equal beta */
    CHECK(dist->slots[0].slot_start == 0, "Single slot: starts at 0");
    CHECK(dist->slots[0].slot_end   == (1 << beta) - 1, "Single slot: ends at 2^beta - 1");

    const uint8_t key[32] = {7};
    MeteorPRNG prng;
    prng_init_raw(&prng, key);

    uint8_t msg_bits[4] = {1, 0, 1, 0};
    MeteorStepResult step = meteor_encode_step(dist, msg_bits, 0, 4, &prng, beta);
    CHECK(step.cp_len == beta, "Single slot: cp_len == beta");

    meteor_free_dist(dist);
    prng_wipe(&prng);
}

int main(void)
{
    if (sodium_init() < 0) {
        fprintf(stderr, "sodium_init() failed\n");
        return 1;
    }

    test_slot_coverage();
    test_encode_decode_roundtrip_mock();
    test_eow_token();
    test_single_slot_full_bits();

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
