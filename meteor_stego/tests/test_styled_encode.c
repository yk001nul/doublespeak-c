/*
 * Styled-encode test: exercises the MeteorStyle embellishment modes.
 * Encodes a short message with each style, prints the full covertext, then
 * decodes and verifies the message is recovered correctly.
 * Requires a running llama-server on http://127.0.0.1:8080.
 * Skipped (exit 0) if the server is not reachable.
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

static void test_style(const char*  label,
                       MeteorStyle  style,
                       const char*  topic,
                       const char*  message_str)
{
    printf("\n── %s ──\n", label);
    printf("  topic   : %s\n", topic);
    printf("  message : %s\n", message_str);

    const uint8_t key_input[] = "styled-encode-test-key";
    uint8_t salt[32] = {
        0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
        0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,
        0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,
        0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,0x20,
    };

    MeteorConfig cfg = {
        .key_raw        = NULL,
        .key_input      = key_input,
        .key_input_len  = sizeof(key_input) - 1,
        .salt           = salt,
        .salt_len       = 32,
        .beta           = 3,
        .num_candidates = 8,  /* 2^beta=2^3=8: fills all slots, 3 bits/phrase */
        .llm_url        = "http://127.0.0.1:8080",
        .hyphen_dict    = NULL,
        .max_steps      = 256,
        .llm_timeout_ms = 30000,
        .style          = style,
    };

    MeteorCtx* ctx = meteor_create(&cfg);
    CHECK(ctx != NULL, "meteor_create: non-NULL");
    if (!ctx) return;

    int   enc_err;
    char* covertext = meteor_encode(ctx,
                                    (const uint8_t*)message_str,
                                    strlen(message_str),
                                    topic,
                                    &enc_err);
    CHECK(enc_err == METEOR_OK, "meteor_encode: no error");
    CHECK(covertext != NULL,    "meteor_encode: covertext non-NULL");

    if (!covertext) { meteor_destroy(ctx); return; }

    printf("  covertext: %s\n", covertext);

    int     dec_err;
    size_t  rec_len;
    uint8_t* recovered = meteor_decode(ctx, covertext, topic, &rec_len, &dec_err);
    CHECK(dec_err == METEOR_OK,             "meteor_decode: no error");
    CHECK(recovered != NULL,                "meteor_decode: recovered non-NULL");
    CHECK(rec_len == strlen(message_str),   "meteor_decode: length matches");
    if (recovered && rec_len == strlen(message_str))
        CHECK(memcmp(recovered, message_str, rec_len) == 0,
              "meteor_decode: content matches original");

    meteor_free(covertext);
    meteor_free(recovered);
    meteor_destroy(ctx);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);  /* disable stdout buffering for live output */

    if (!server_reachable()) {
        printf("llama-server not reachable — skipping styled encode tests.\n");
        return 0;
    }

    printf("Server reachable — running styled encode tests...\n");

    test_style("INFORMAL_CHAT — commute sentence",
               METEOR_STYLE_INFORMAL_CHAT,
               "John goes to the office using his car every morning.",
               "hi");

    test_style("NEWS_ARTICLE — policy sentence",
               METEOR_STYLE_NEWS_ARTICLE,
               "The government announced new policies to reduce carbon emissions by 2030.",
               "hi");

    test_style("CASUAL_BLOG — weekend sentence",
               METEOR_STYLE_CASUAL_BLOG,
               "Sarah spent the whole weekend hiking in the mountains with her dog.",
               "hi");

    test_style("FORMAL_EMAIL — meeting sentence",
               METEOR_STYLE_FORMAL_EMAIL,
               "The team will present the quarterly results to stakeholders on Friday.",
               "hi");

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
