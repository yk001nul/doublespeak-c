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

#ifdef _WIN32
#include <windows.h>
#define sleep_sec(n) Sleep((n) * 1000)
static long long wall_ms(void) { return (long long)GetTickCount64(); }
#else
#include <unistd.h>
#include <time.h>
#define sleep_sec(n) sleep(n)
static long long wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}
#endif

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { if (cond) { printf("[PASS] %s\n", msg); passed++; } \
         else { printf("[FAIL] %s  (line %d)\n", msg, __LINE__); failed++; } } while(0)

static MeteorChatTemplate get_chat_template(void)
{
    const char* t = getenv("METEOR_CHAT_TEMPLATE");
    if (t && strcmp(t, "chatml") == 0) return METEOR_TEMPLATE_CHATML;
    return METEOR_TEMPLATE_PHI3;
}

static int server_reachable(void)
{
    LLMClient* c = llm_client_create("http://127.0.0.1:8080", 6, 3000,
                                      get_chat_template());
    if (!c) return 0;
    int ok = llm_client_health(c);
    llm_client_destroy(c);
    return ok;
}

/*
 * Clear accumulated server state between encode and decode phases.
 *
 * Primary path: POST /slots/0 {"action":"erase"} — clears the slot KV cache
 * without restarting the process (~ms).
 *
 * Fallback: if METEOR_SERVER_RESTART_CMD is set and the erase fails (e.g. the
 * endpoint is disabled), run the restart command and poll /health.
 *
 * Both paths print elapsed time so results can be compared.
 */
static void reset_server_state(void)
{
    long long t0 = wall_ms();
    LLMClient* c = llm_client_create("http://127.0.0.1:8080", 6, 3000,
                                      get_chat_template());
    int erased = c ? llm_client_erase_slot(c, 0) : 0;
    if (c) llm_client_destroy(c);
    long long erase_ms = wall_ms() - t0;

    if (erased) {
        printf("[slot-erase] slot 0 erased in %lld ms\n", erase_ms);
        fflush(stdout);
        return;
    }

    fprintf(stderr, "[slot-erase] failed (%lld ms)", erase_ms);
    const char* cmd = getenv("METEOR_SERVER_RESTART_CMD");
    if (!cmd) {
        fprintf(stderr, " — no fallback configured, continuing\n");
        return;
    }
    fprintf(stderr, " — falling back to server restart\n");
    fflush(stderr);

    long long t1 = wall_ms();
    printf("[restart] running: %s\n", cmd);
    fflush(stdout);
    int rc = system(cmd);
    if (rc != 0)
        fprintf(stderr, "[restart] command exited %d — continuing anyway\n", rc);

    printf("[restart] polling for server health...\n");
    fflush(stdout);
    for (int i = 0; i < 60; i++) {
        sleep_sec(5);
        LLMClient* hc = llm_client_create("http://127.0.0.1:8080", 6, 3000,
                                           get_chat_template());
        if (hc) {
            int ok = llm_client_health(hc);
            llm_client_destroy(hc);
            if (ok) {
                printf("[restart] server ready in %lld ms\n", wall_ms() - t1);
                fflush(stdout);
                return;
            }
        }
    }
    fprintf(stderr, "[restart] server did not become healthy after 5 min\n");
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
        .chat_template  = get_chat_template(),
    };

    MeteorCtx* ctx = meteor_create(&cfg);
    CHECK(ctx != NULL, "meteor_create: non-NULL");
    if (!ctx) return;

    long long t_enc = wall_ms();
    int   enc_err;
    char* covertext = meteor_encode(ctx,
                                    (const uint8_t*)message_str,
                                    strlen(message_str),
                                    context,
                                    &enc_err);
    long long enc_ms = wall_ms() - t_enc;

    CHECK(enc_err == METEOR_OK, "meteor_encode: no error");
    CHECK(covertext != NULL,    "meteor_encode: covertext non-NULL");
    if (!covertext) { meteor_destroy(ctx); return; }
    printf("  covertext: %.80s%s\n", covertext, strlen(covertext) > 80 ? "..." : "");
    printf("  encode:    %lld ms\n", enc_ms);

    reset_server_state();

    long long t_dec = wall_ms();
    int      dec_err;
    size_t   rec_len;
    uint8_t* recovered = meteor_decode(ctx, covertext, context, &rec_len, &dec_err);
    long long dec_ms = wall_ms() - t_dec;

    CHECK(dec_err == METEOR_OK,              "meteor_decode: no error");
    CHECK(recovered != NULL,                 "meteor_decode: recovered non-NULL");
    CHECK(rec_len   == strlen(message_str),  "meteor_decode: length matches");
    if (recovered && rec_len == strlen(message_str))
        CHECK(memcmp(recovered, message_str, rec_len) == 0,
              "meteor_decode: content matches original message");
    printf("  decode:    %lld ms\n", dec_ms);

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
