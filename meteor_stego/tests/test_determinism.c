/*
 * Determinism test: two calls with identical arguments must produce identical
 * slot assignments from the LLM.
 * Requires a running llama-server on http://127.0.0.1:8080.
 */
#include "../src/llm_client.h"
#include "../src/meteor_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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

static void test_dist_determinism(const char* context, const char* partial, int is_new)
{
    LLMClient* c = llm_client_create("http://127.0.0.1:8080", 6, 30000);
    if (!c) { printf("[SKIP] client creation failed\n"); return; }

    LLMResponse* r1 = llm_client_get_syllable_dist(c, NULL, context, partial, is_new);
    LLMResponse* r2 = llm_client_get_syllable_dist(c, NULL, context, partial, is_new);

    CHECK(r1 != NULL && r2 != NULL, "Determinism: both responses non-NULL");

    if (r1 && r2) {
        CHECK(r1->count == r2->count, "Determinism: same candidate count");

        /* build distributions and compare slot assignments */
        int beta = 3;
        const char** t1 = (const char**)malloc((size_t)r1->count * sizeof(char*));
        float*        p1 = (float*)malloc((size_t)r1->count * sizeof(float));
        const char** t2 = (const char**)malloc((size_t)r2->count * sizeof(char*));
        float*        p2 = (float*)malloc((size_t)r2->count * sizeof(float));

        for (int i = 0; i < r1->count; i++) { t1[i] = r1->candidates[i].text; p1[i] = r1->candidates[i].prob; }
        for (int i = 0; i < r2->count; i++) { t2[i] = r2->candidates[i].text; p2[i] = r2->candidates[i].prob; }

        MeteorDist* d1 = meteor_build_dist(t1, p1, r1->count, beta);
        MeteorDist* d2 = meteor_build_dist(t2, p2, r2->count, beta);

        int slots_match = 1;
        if (d1 && d2 && d1->count == d2->count) {
            for (int i = 0; i < d1->count; i++) {
                if (d1->slots[i].slot_start != d2->slots[i].slot_start ||
                    d1->slots[i].slot_end   != d2->slots[i].slot_end) {
                    slots_match = 0; break;
                }
            }
        } else {
            slots_match = 0;
        }
        CHECK(slots_match, "Determinism: slot assignments are identical");

        meteor_free_dist(d1); meteor_free_dist(d2);
        free(t1); free(p1); free(t2); free(p2);
    }

    llm_response_free(r1);
    llm_response_free(r2);
    llm_client_destroy(c);
}

int main(void)
{
    if (!server_reachable()) {
        printf("llama-server not reachable — skipping determinism tests.\n");
        return 0;
    }

    printf("Running determinism tests...\n\n");

    test_dist_determinism("The report stated", "",   1);
    test_dist_determinism("The report stated that",  "re", 0);

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
