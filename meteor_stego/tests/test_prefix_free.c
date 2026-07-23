/*
 * Prefix-free candidate-set invariant.
 *
 * Both decode paths recover the encoder's choice by taking the LONGEST
 * candidate that is a prefix of the remaining covertext (decode.c:377-387
 * syllable, :254-262 style). That is only correct if the candidate set is a
 * uniquely decodable code: if the encoder picks a candidate that is a proper
 * prefix of another candidate in the same slot table, the decoder takes the
 * longer one and every subsequent bit is garbage, with no error signal.
 *
 * parse_llm_response() therefore filters the model's keys down to a prefix-free
 * set. This test drives it with crafted JSON — no llama-server, no model — so
 * the invariant is checked directly rather than inferred from a round trip.
 *
 * Why that matters here: the round-trip tests cannot be trusted to catch this.
 * decode(encode(m)) == m held for years while the deleted fallback tables were
 * answering every call, and those tables happened to be prefix-free, so the bug
 * was unreachable. A test that only asserts the round trip would have passed
 * throughout. See the "No LLM fallbacks" section in CLAUDE.md.
 */
#include "../src/llm_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { if (cond) { printf("[PASS] %s\n", msg); passed++; } \
         else { printf("[FAIL] %s  (line %d)\n", msg, __LINE__); failed++; } } while(0)

/* Wraps candidate JSON in the /completion envelope parse_llm_response expects.
   The inner object is a JSON string, so its quotes are escaped. */
static char* envelope(const char* inner)
{
    size_t n   = strlen(inner) * 2 + 64;
    char*  buf = (char*)malloc(n);
    if (!buf) return NULL;
    size_t o = 0;
    const char* pre = "{\"content\":\"";
    memcpy(buf + o, pre, strlen(pre)); o += strlen(pre);
    for (const char* p = inner; *p; p++) {
        if (*p == '"') buf[o++] = '\\';
        buf[o++] = *p;
    }
    const char* post = "\"}";
    memcpy(buf + o, post, strlen(post)); o += strlen(post);
    buf[o] = '\0';
    return buf;
}

/* The property under test: no candidate is a prefix of any other. */
static int is_prefix_free(const LLMResponse* r)
{
    for (int a = 0; a < r->count; a++) {
        for (int b = 0; b < r->count; b++) {
            if (a == b) continue;
            size_t la = strlen(r->candidates[a].text);
            size_t lb = strlen(r->candidates[b].text);
            size_t m  = la < lb ? la : lb;
            if (memcmp(r->candidates[a].text, r->candidates[b].text, m) == 0)
                return 0;
        }
    }
    return 1;
}

static LLMResponse* parse_inner(const char* inner, int max_candidates)
{
    char* env = envelope(inner);
    if (!env) return NULL;
    LLMResponse* r = parse_llm_response(env, max_candidates);
    free(env);
    return r;
}

static int has_text(const LLMResponse* r, const char* want)
{
    for (int i = 0; i < r->count; i++)
        if (strcmp(r->candidates[i].text, want) == 0) return 1;
    return 0;
}

/* The exact shape that desynced syllable-mode decode: "re" is a proper prefix
   of "res" and "reser", so all three could occupy slots at once. */
static void test_drops_proper_prefixes(void)
{
    LLMResponse* r = parse_inner(
        "{\"re\": 0.4, \"res\": 0.3, \"reser\": 0.2, \"tion\": 0.1}", 8);
    CHECK(r != NULL, "proper prefixes: parse succeeds");
    if (!r) return;

    CHECK(is_prefix_free(r), "proper prefixes: result is prefix-free");
    CHECK(has_text(r, "re"),   "proper prefixes: keeps the earliest key (re)");
    CHECK(!has_text(r, "res"), "proper prefixes: drops res");
    CHECK(!has_text(r, "reser"), "proper prefixes: drops reser");
    CHECK(has_text(r, "tion"), "proper prefixes: keeps non-conflicting tion");
    CHECK(r->count == 2, "proper prefixes: exactly 2 candidates survive");
    llm_response_free(r);
}

/* A longer key arriving BEFORE its own prefix must also be resolved — the
   conflict is symmetric, so order decides which survives, not length. */
static void test_conflict_is_symmetric(void)
{
    LLMResponse* r = parse_inner("{\"reser\": 0.6, \"re\": 0.4}", 8);
    CHECK(r != NULL, "symmetric: parse succeeds");
    if (!r) return;

    CHECK(is_prefix_free(r), "symmetric: result is prefix-free");
    CHECK(has_text(r, "reser"), "symmetric: keeps the earliest key (reser)");
    CHECK(!has_text(r, "re"),   "symmetric: drops the later prefix re");
    CHECK(r->count == 1, "symmetric: exactly 1 candidate survives");
    llm_response_free(r);
}

/* Exact duplicates are the |a| == |b| case of the same rule. */
static void test_exact_duplicates(void)
{
    LLMResponse* r = parse_inner(
        "{\"ing\": 0.5, \"ing\": 0.3, \"ed\": 0.2}", 8);
    CHECK(r != NULL, "duplicates: parse succeeds");
    if (!r) return;

    CHECK(is_prefix_free(r), "duplicates: result is prefix-free");
    CHECK(r->count == 2, "duplicates: repeated key collapses to one");
    llm_response_free(r);
}

/* Non-overlapping keys must survive untouched — the filter must not be
   over-eager, or capacity collapses for no reason. */
static void test_keeps_disjoint_candidates(void)
{
    LLMResponse* r = parse_inner(
        "{\"pro\": 0.3, \"con\": 0.3, \"de\": 0.2, \"un\": 0.2}", 8);
    CHECK(r != NULL, "disjoint: parse succeeds");
    if (!r) return;

    CHECK(is_prefix_free(r), "disjoint: result is prefix-free");
    CHECK(r->count == 4, "disjoint: all 4 candidates survive");
    llm_response_free(r);
}

/* The EOW marker is "\xc2\xb7" on the wire and \x01 internally. Words can only
   terminate if it survives filtering, so a syllable must never collide with it.
   It cannot: syllables are [a-z]+ and \x01 differs in the first byte. */
static void test_eow_survives(void)
{
    LLMResponse* r = parse_inner(
        "{\"a\": 0.4, \"\xc2\xb7\": 0.4, \"ing\": 0.2}", 8);
    CHECK(r != NULL, "eow: parse succeeds");
    if (!r) return;

    CHECK(is_prefix_free(r), "eow: result is prefix-free");
    CHECK(has_text(r, "\x01"), "eow: EOW marker survives the filter");
    CHECK(has_text(r, "a"),    "eow: single-letter syllable survives alongside EOW");
    llm_response_free(r);
}

/* Style-mode candidates are whole phrases and hit the same rule. */
static void test_phrase_candidates(void)
{
    LLMResponse* r = parse_inner(
        "{\"to the office\": 0.5, \"to the office today\": 0.3, \"by the river\": 0.2}", 8);
    CHECK(r != NULL, "phrases: parse succeeds");
    if (!r) return;

    CHECK(is_prefix_free(r), "phrases: result is prefix-free");
    CHECK(has_text(r, "to the office"), "phrases: keeps the earliest phrase");
    CHECK(!has_text(r, "to the office today"), "phrases: drops the extending phrase");
    CHECK(has_text(r, "by the river"), "phrases: keeps the disjoint phrase");
    llm_response_free(r);
}

/* Surviving probabilities are renormalised, so dropped mass is not lost. */
static void test_probabilities_renormalised(void)
{
    LLMResponse* r = parse_inner("{\"re\": 0.5, \"res\": 0.3, \"tion\": 0.5}", 8);
    CHECK(r != NULL, "probs: parse succeeds");
    if (!r) return;

    float sum = 0.0f;
    for (int i = 0; i < r->count; i++) sum += r->candidates[i].prob;
    CHECK(sum > 0.999f && sum < 1.001f, "probs: surviving probabilities sum to 1.0");
    llm_response_free(r);
}

int main(void)
{
    printf("Prefix-free candidate-set invariant (no server required)\n\n");

    test_drops_proper_prefixes();
    test_conflict_is_symmetric();
    test_exact_duplicates();
    test_keeps_disjoint_candidates();
    test_eow_survives();
    test_phrase_candidates();
    test_probabilities_renormalised();

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
