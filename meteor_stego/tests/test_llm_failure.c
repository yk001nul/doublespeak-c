/*
 * LLM-failure tests: an unreachable server must FAIL, not degrade.
 *
 * Why this exists: llm_client.c used to answer a failed /completion with a
 * uniform distribution over a hardcoded 20-entry table. Both sides substituted
 * the same table deterministically, so the PRNG stayed in lockstep and the
 * round-trip still succeeded — a total LLM outage reported SUCCESS and produced
 * covertext drawn from a 20-phrase vocabulary. Every existing test passed
 * throughout, because roundtrip/determinism/styled_encode only check that
 * decode(encode(m)) == m, which a shared fallback satisfies perfectly.
 *
 * That made the covertext trivially machine-detectable, defeating the whole
 * point of the library, so the fallbacks were removed. These tests are the
 * regression guard: they need no llama-server and no model, and they are the
 * only tests that can tell "working" apart from "silently degraded".
 *
 * Field signature of the old bug, for anyone diagnosing this from live data:
 * every step yields exactly `beta` bits (a uniform 2^beta-candidate
 * distribution fills every slot), so a run takes exactly ceil(total_bits/beta)
 * steps, and wall-clock per step pins to llm_timeout_ms.
 */
#include "../include/meteor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { if (cond) { printf("[PASS] %s\n", msg); passed++; } \
         else { printf("[FAIL] %s  (line %d)\n", msg, __LINE__); failed++; } } while(0)

/* Port 1 is privileged and never served by this project, so a connect attempt
   is refused immediately rather than hanging until llm_timeout_ms. */
#define DEAD_URL "http://127.0.0.1:1"

static const uint8_t KEY_INPUT[] = "my-shared-secret-passphrase";
static uint8_t SALT[32] = {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
    17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32
};

static MeteorCtx* make_ctx(int style)
{
    MeteorConfig cfg = {
        .key_raw        = NULL,
        .key_input      = KEY_INPUT,
        .key_input_len  = sizeof(KEY_INPUT) - 1,
        .salt           = SALT,
        .salt_len       = 32,
        .beta           = 3,
        .num_candidates = 8,
        .llm_url        = DEAD_URL,
        .hyphen_dict    = NULL,
        .max_steps      = 256,
        .llm_timeout_ms = 2000,
        .style          = style,
    };
    return meteor_create(&cfg);
}

/* An unreachable server must abort the encode with METEOR_ERR_LLM and hand back
   no covertext at all — not a truncated one, which cannot round-trip and would
   be the same silent degradation in another guise. */
static void test_encode_fails(int style, const char* label)
{
    MeteorCtx* ctx = make_ctx(style);
    CHECK(ctx != NULL, "meteor_create with dead LLM URL still succeeds");
    if (!ctx) return;

    int   err       = METEOR_OK;
    char* covertext = meteor_encode(ctx, (const uint8_t*)"hi", 2,
                                    "John goes to the office every morning.",
                                    &err);

    printf("  [%s] err=%d covertext=%s\n", label, err,
           covertext ? covertext : "(null)");
    CHECK(covertext == NULL, "encode returns NULL when the LLM is unreachable");
    CHECK(err == METEOR_ERR_LLM, "encode reports METEOR_ERR_LLM");

    free(covertext);
    meteor_destroy(ctx);
}

static void test_decode_fails(int style, const char* label)
{
    MeteorCtx* ctx = make_ctx(style);
    if (!ctx) { CHECK(0, "meteor_create"); return; }

    int      err     = METEOR_OK;
    size_t   msg_len = 12345;            /* poisoned: must be reset on failure */
    uint8_t* msg     = meteor_decode(ctx,
                                     "and the world from the start. in the morning.",
                                     "John goes to the office every morning.",
                                     &msg_len, &err);

    printf("  [%s] err=%d msg=%p len=%zu\n", label, err, (void*)msg, msg_len);
    CHECK(msg == NULL, "decode returns NULL when the LLM is unreachable");
    CHECK(err == METEOR_ERR_LLM, "decode reports METEOR_ERR_LLM");
    CHECK(msg_len == 0, "decode zeroes out_msg_len on failure");

    free(msg);
    meteor_destroy(ctx);
}

int main(void)
{
    printf("=== LLM failure tests (no llama-server required) ===\n");

    /* Style mode (phrase-level) and legacy syllable mode take different code
       paths through llm_client.c, and each had its own fallback table. */
    printf("\n-- style mode (phrase-level) --\n");
    test_encode_fails(1, "style");
    test_decode_fails(1, "style");

    printf("\n-- legacy syllable mode --\n");
    test_encode_fails(0, "syllable");
    test_decode_fails(0, "syllable");

    printf("\n=== %d passed, %d failed ===\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
