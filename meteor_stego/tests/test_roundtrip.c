/*
 * Full encode → decode roundtrip test.
 * Requires a running llama-server on http://127.0.0.1:8080.
 * Skipped (exit 0 with a note) if server is not reachable.
 */
#include "../include/meteor.h"
#include "../src/llm_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { if (cond) { printf("[PASS] %s\n", msg); passed++; } \
         else { printf("[FAIL] %s  (line %d)\n", msg, __LINE__); failed++; } } while(0)

static int server_reachable(void)
{
    LLMClient* c = llm_client_create("http://127.0.0.1:8080", 6, 3000);
    if (!c) return 0;
    int ok = llm_client_health(c);
    llm_client_destroy(c);
    return ok;
}

static void test_roundtrip(const char* message_str, const char* context)
{
    const uint8_t key_input[] = "my-shared-secret-passphrase";
    uint8_t       salt[32]    = {
        1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
        17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32
    };

    MeteorConfig cfg = {
        .key_raw        = NULL,
        .key_input      = key_input,
        .key_input_len  = sizeof(key_input) - 1,
        .salt           = salt,
        .salt_len       = 32,
        .beta           = 3,
        .num_candidates = 6,
        .llm_url        = "http://127.0.0.1:8080",
        .hyphen_dict    = NULL,
        .max_steps      = 256,
        .llm_timeout_ms = 30000,
    };

    MeteorCtx* ctx = meteor_create(&cfg);
    CHECK(ctx != NULL, "meteor_create: non-NULL");
    if (!ctx) return;

    int    enc_err;
    char*  covertext = meteor_encode(ctx,
                                     (const uint8_t*)message_str,
                                     strlen(message_str),
                                     context,
                                     &enc_err);
    CHECK(enc_err == METEOR_OK, "meteor_encode: no error");
    CHECK(covertext != NULL,    "meteor_encode: covertext non-NULL");

    if (!covertext) { meteor_destroy(ctx); return; }

    printf("  covertext: %.80s%s\n", covertext, strlen(covertext) > 80 ? "..." : "");

    int     dec_err;
    size_t  rec_len;
    uint8_t* recovered = meteor_decode(ctx, covertext, context, &rec_len, &dec_err);
    CHECK(dec_err == METEOR_OK,              "meteor_decode: no error");
    CHECK(recovered != NULL,                 "meteor_decode: recovered non-NULL");
    CHECK(rec_len   == strlen(message_str),  "meteor_decode: length matches");
    if (recovered && rec_len == strlen(message_str))
        CHECK(memcmp(recovered, message_str, rec_len) == 0,
              "meteor_decode: content matches original message");

    meteor_free(covertext);
    meteor_free(recovered);
    meteor_destroy(ctx);
}

int main(void)
{
    if (!server_reachable()) {
        printf("llama-server not reachable — skipping roundtrip tests.\n");
        printf("Start the server with: build/start_llama_server.sh <model.gguf>\n");
        return 0; /* skip, not fail */
    }

    printf("Server reachable — running roundtrip tests...\n\n");

    test_roundtrip("hi",     "The report stated");
    test_roundtrip("secret", "Researchers found");

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
