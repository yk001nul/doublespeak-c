/*
 * Capacity estimation tests.
 *
 * Heuristic tests (no LLM): always run.
 * LLM-sampled tests: require a running llama-server on http://127.0.0.1:8080.
 *   Skipped (exit 0) if the server is not reachable — safe for CI/CD.
 */
#include "../include/meteor.h"
#include "../src/llm_client.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { if (cond) { printf("[PASS] %s\n", msg); passed++; } \
         else { printf("[FAIL] %s  (line %d)\n", msg, __LINE__); failed++; } } while(0)

#define CHECK_EQ(a, b, msg) \
    do { int _a = (a), _b = (b); \
         if (_a == _b) { printf("[PASS] %s  (%d)\n", msg, _a); passed++; } \
         else { printf("[FAIL] %s  (got %d, expected %d, line %d)\n", msg, _a, _b, __LINE__); failed++; } } while(0)

static int server_reachable(void)
{
    LLMClient* c = llm_client_create("http://127.0.0.1:8080", 6, 3000);
    if (!c) return 0;
    int ok = llm_client_health(c);
    llm_client_destroy(c);
    return ok;
}

static MeteorCtx* make_ctx(MeteorStyle style)
{
    static const uint8_t key_input[] = "capacity-test-key";
    static uint8_t salt[32] = {
        0x10,0x20,0x30,0x40,0x50,0x60,0x70,0x80,
        0x90,0xa0,0xb0,0xc0,0xd0,0xe0,0xf0,0x01,
        0x11,0x21,0x31,0x41,0x51,0x61,0x71,0x81,
        0x91,0xa1,0xb1,0xc1,0xd1,0xe1,0xf1,0x02,
    };
    MeteorConfig cfg = {
        .key_input      = key_input,
        .key_input_len  = sizeof(key_input) - 1,
        .salt           = salt,
        .salt_len       = 32,
        .beta           = 3,
        .num_candidates = 6,
        .llm_url        = "http://127.0.0.1:8080",
        .max_steps      = 256,
        .llm_timeout_ms = 30000,
        .style          = style,
    };
    return meteor_create(&cfg);
}

/* ── Heuristic tests (no LLM) ────────────────────────────────────────────── */

static void test_heuristic_basic(void)
{
    printf("\n── Heuristic: basic INFORMAL_CHAT ──\n");

    MeteorCtx* ctx = make_ctx(METEOR_STYLE_INFORMAL_CHAT);
    CHECK(ctx != NULL, "meteor_create: non-NULL");
    if (!ctx) return;

    MeteorCapacityEstimate est;
    int rc = meteor_estimate_capacity(ctx, "i like burger. burger good.", 0, &est);

    CHECK_EQ(rc, METEOR_OK,       "return code METEOR_OK");
    CHECK_EQ(est.sample_steps_used, 0, "sample_steps_used is 0 (heuristic)");

    /* "i like burger. burger good." = 5 words × 1.4 expansion = 7 */
    CHECK_EQ(est.estimated_words, 7, "estimated_words = 7");

    /* avg_bits_per_word must be beta × 0.65 = 1.95 */
    CHECK(fabsf(est.avg_bits_per_word - 1.95f) < 0.01f,
          "avg_bits_per_word ≈ 1.95 (beta=3 × 0.65)");

    /* estimated_bits = floor(7 × 1.95) = 13 */
    CHECK_EQ(est.estimated_bits, 13, "estimated_bits = 13");

    /* estimated_bytes = 13 / 8 = 1 */
    CHECK_EQ(est.estimated_bytes, 1, "estimated_bytes = 1");

    printf("  estimated_words=%d bits=%d bytes=%d avg=%.3f\n",
           est.estimated_words, est.estimated_bits,
           est.estimated_bytes, est.avg_bits_per_word);

    meteor_destroy(ctx);
}

static void test_heuristic_news(void)
{
    printf("\n── Heuristic: NEWS_ARTICLE ──\n");

    MeteorCtx* ctx = make_ctx(METEOR_STYLE_NEWS_ARTICLE);
    CHECK(ctx != NULL, "meteor_create: non-NULL");
    if (!ctx) return;

    /* 7 words × 1.1 expansion = 7 (rounds to 8 at 7.7) */
    const char* topic = "global temperatures rising, scientists warn of consequences";
    MeteorCapacityEstimate est;
    int rc = meteor_estimate_capacity(ctx, topic, 0, &est);

    CHECK_EQ(rc, METEOR_OK, "return code METEOR_OK");
    CHECK(est.estimated_words > 0,   "estimated_words > 0");
    CHECK(est.estimated_bits  > 0,   "estimated_bits > 0");
    CHECK(est.estimated_bytes >= 0,  "estimated_bytes >= 0");
    CHECK_EQ(est.sample_steps_used, 0, "sample_steps_used = 0");

    printf("  topic word count≈7, estimated_words=%d bits=%d bytes=%d\n",
           est.estimated_words, est.estimated_bits, est.estimated_bytes);

    meteor_destroy(ctx);
}

static void test_heuristic_proportional(void)
{
    printf("\n── Heuristic: longer context → more capacity ──\n");

    MeteorCtx* ctx = make_ctx(METEOR_STYLE_CASUAL_BLOG);
    CHECK(ctx != NULL, "meteor_create: non-NULL");
    if (!ctx) return;

    MeteorCapacityEstimate short_est, long_est;
    meteor_estimate_capacity(ctx, "hello world", 0, &short_est);
    meteor_estimate_capacity(ctx,
        "hello world this is a much longer sentence with many more words in it",
        0, &long_est);

    CHECK(long_est.estimated_words > short_est.estimated_words,
          "longer context → more estimated_words");
    CHECK(long_est.estimated_bits  > short_est.estimated_bits,
          "longer context → more estimated_bits");

    printf("  short: words=%d bits=%d  |  long: words=%d bits=%d\n",
           short_est.estimated_words, short_est.estimated_bits,
           long_est.estimated_words,  long_est.estimated_bits);

    meteor_destroy(ctx);
}

static void test_heuristic_style_expansion(void)
{
    printf("\n── Heuristic: style expansion factors ──\n");

    const char* topic = "the quick brown fox jumps"; /* 5 words */

    MeteorCtx* chat  = make_ctx(METEOR_STYLE_INFORMAL_CHAT);  /* ×1.4 → 7 */
    MeteorCtx* news  = make_ctx(METEOR_STYLE_NEWS_ARTICLE);   /* ×1.1 → 6 */
    MeteorCtx* blog  = make_ctx(METEOR_STYLE_CASUAL_BLOG);    /* ×1.3 → 7 */
    MeteorCtx* email = make_ctx(METEOR_STYLE_FORMAL_EMAIL);   /* ×1.2 → 6 */

    MeteorCapacityEstimate e_chat, e_news, e_blog, e_email;
    meteor_estimate_capacity(chat,  topic, 0, &e_chat);
    meteor_estimate_capacity(news,  topic, 0, &e_news);
    meteor_estimate_capacity(blog,  topic, 0, &e_blog);
    meteor_estimate_capacity(email, topic, 0, &e_email);

    /* INFORMAL_CHAT and CASUAL_BLOG both round to 7; NEWS_ARTICLE rounds to 6 */
    CHECK(e_chat.estimated_words >= e_news.estimated_words,
          "INFORMAL_CHAT words >= NEWS_ARTICLE words (higher expansion)");
    CHECK(e_chat.estimated_bits  >= e_news.estimated_bits,
          "INFORMAL_CHAT bits  >= NEWS_ARTICLE bits");

    printf("  CHAT=%d BLOG=%d EMAIL=%d NEWS=%d words\n",
           e_chat.estimated_words, e_blog.estimated_words,
           e_email.estimated_words, e_news.estimated_words);

    meteor_destroy(chat); meteor_destroy(news);
    meteor_destroy(blog); meteor_destroy(email);
}

static void test_heuristic_null_inputs(void)
{
    printf("\n── Heuristic: NULL / empty input guards ──\n");

    MeteorCtx* ctx = make_ctx(METEOR_STYLE_INFORMAL_CHAT);
    CHECK(ctx != NULL, "meteor_create: non-NULL");
    if (!ctx) return;

    MeteorCapacityEstimate est;
    int rc;

    rc = meteor_estimate_capacity(NULL, "hello", 0, &est);
    CHECK_EQ(rc, METEOR_ERR_CONFIG, "NULL ctx → METEOR_ERR_CONFIG");

    rc = meteor_estimate_capacity(ctx, NULL, 0, &est);
    CHECK_EQ(rc, METEOR_ERR_CONFIG, "NULL context → METEOR_ERR_CONFIG");

    rc = meteor_estimate_capacity(ctx, "", 0, &est);
    CHECK_EQ(rc, METEOR_ERR_CONFIG, "empty context → METEOR_ERR_CONFIG");

    rc = meteor_estimate_capacity(ctx, "hello", 0, NULL);
    CHECK_EQ(rc, METEOR_ERR_CONFIG, "NULL out → METEOR_ERR_CONFIG");

    meteor_destroy(ctx);
}

/* ── LLM-sampled tests (skipped in CI/CD) ───────────────────────────────── */

static void test_sampled_capacity(void)
{
    printf("\n── LLM-sampled: INFORMAL_CHAT burger (3 samples) ──\n");

    MeteorCtx* ctx = make_ctx(METEOR_STYLE_INFORMAL_CHAT);
    CHECK(ctx != NULL, "meteor_create: non-NULL");
    if (!ctx) return;

    MeteorCapacityEstimate est;
    int rc = meteor_estimate_capacity(ctx,
                                      "i like burger. burger good.",
                                      3, &est);

    CHECK_EQ(rc, METEOR_OK,              "return code METEOR_OK");
    CHECK_EQ(est.sample_steps_used, 3,   "sample_steps_used = 3");
    CHECK(est.avg_bits_per_word > 0.0f,  "avg_bits_per_word > 0");
    CHECK(est.avg_bits_per_word <= 3.0f, "avg_bits_per_word <= beta (3)");
    CHECK(est.estimated_words   > 0,     "estimated_words > 0");
    CHECK(est.estimated_bits    > 0,     "estimated_bits > 0");

    printf("  sampled avg_bits_per_word=%.3f  estimated: words=%d bits=%d bytes=%d\n",
           est.avg_bits_per_word, est.estimated_words,
           est.estimated_bits, est.estimated_bytes);

    meteor_destroy(ctx);
}

static void test_sampled_news(void)
{
    printf("\n── LLM-sampled: NEWS_ARTICLE climate (3 samples) ──\n");

    MeteorCtx* ctx = make_ctx(METEOR_STYLE_NEWS_ARTICLE);
    CHECK(ctx != NULL, "meteor_create: non-NULL");
    if (!ctx) return;

    MeteorCapacityEstimate est;
    int rc = meteor_estimate_capacity(ctx,
        "global temperatures rising, scientists warn of consequences",
        3, &est);

    CHECK_EQ(rc, METEOR_OK,              "return code METEOR_OK");
    CHECK_EQ(est.sample_steps_used, 3,   "sample_steps_used = 3");
    CHECK(est.avg_bits_per_word >= 0.0f, "avg_bits_per_word >= 0");
    CHECK(est.avg_bits_per_word <= 3.0f, "avg_bits_per_word <= beta (3)");

    printf("  sampled avg_bits_per_word=%.3f  estimated: words=%d bits=%d bytes=%d\n",
           est.avg_bits_per_word, est.estimated_words,
           est.estimated_bits, est.estimated_bytes);

    meteor_destroy(ctx);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("=== Heuristic capacity tests (no LLM required) ===\n");
    test_heuristic_basic();
    test_heuristic_news();
    test_heuristic_proportional();
    test_heuristic_style_expansion();
    test_heuristic_null_inputs();

    printf("\n=== LLM-sampled capacity tests ===\n");
    if (!server_reachable()) {
        printf("llama-server not reachable — skipping LLM-sampled tests.\n");
    } else {
        test_sampled_capacity();
        test_sampled_news();
    }

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
