/*
 * doublespeak CLI tests.
 *
 * Group A: argument-parsing permutations. Fast, no server needed, always run.
 * Group B: a real encode -> decode round trip through doublespeak_run(),
 *          gated on llama-server being reachable (same convention as
 *          test_roundtrip.c etc.) — skipped, not failed, if it isn't.
 */
#include "../apps/doublespeak.h"
#include "../include/meteor.h"
#include "../src/llm_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef METEOR_TEST_FIXTURES_DIR
#define METEOR_TEST_FIXTURES_DIR "."
#endif

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { if (cond) { printf("[PASS] %s\n", msg); passed++; } \
         else { printf("[FAIL] %s  (line %d)\n", msg, __LINE__); failed++; } } while(0)

/* ── Group A: argument-parsing permutations ─────────────────────────────── */

static void test_minimal(void)
{
    char* argv[] = { "doublespeak", "hi", "ctx", "pass" };
    DoublespeakArgs a;
    int rc = doublespeak_parse_args(4, argv, &a);
    CHECK(rc == 0, "minimal: parse succeeds");
    CHECK(a.is_file == 0 && a.is_decode == 0, "minimal: no flags set");
    CHECK(strcmp(a.context, "ctx") == 0 && strcmp(a.passphrase, "pass") == 0,
          "minimal: context/passphrase captured");
    CHECK(a.style_index == 1, "minimal: style_index defaults to 1");
    CHECK(a.output_path == NULL && a.llm_url == NULL,
          "minimal: outputpath/URL default to NULL");
}

static void test_flag_f(void)
{
    char* argv[] = { "doublespeak", "path.txt", "-f", "ctx", "pass" };
    DoublespeakArgs a;
    int rc = doublespeak_parse_args(5, argv, &a);
    CHECK(rc == 0, "-f alone: parse succeeds");
    CHECK(a.is_file == 1 && a.is_decode == 0, "-f alone: is_file set, is_decode clear");
}

static void test_flag_d(void)
{
    char* argv[] = { "doublespeak", "covertext", "-d", "ctx", "pass" };
    DoublespeakArgs a;
    int rc = doublespeak_parse_args(5, argv, &a);
    CHECK(rc == 0, "-d alone: parse succeeds");
    CHECK(a.is_file == 0 && a.is_decode == 1, "-d alone: is_decode set, is_file clear");
}

static void test_flags_f_then_d(void)
{
    char* argv[] = { "doublespeak", "path.txt", "-f", "-d", "ctx", "pass" };
    DoublespeakArgs a;
    int rc = doublespeak_parse_args(6, argv, &a);
    CHECK(rc == 0, "-f -d: parse succeeds");
    CHECK(a.is_file == 1 && a.is_decode == 1, "-f -d: both flags set");
}

static void test_flags_d_then_f(void)
{
    char* argv[] = { "doublespeak", "path.txt", "-d", "-f", "ctx", "pass" };
    DoublespeakArgs a;
    int rc = doublespeak_parse_args(6, argv, &a);
    CHECK(rc == 0, "-d -f: parse succeeds");
    CHECK(a.is_file == 1 && a.is_decode == 1, "-d -f: both flags set (order independent)");
}

static void test_style_index(void)
{
    char* argv[] = { "doublespeak", "hi", "ctx", "pass", "3" };
    DoublespeakArgs a;
    int rc = doublespeak_parse_args(5, argv, &a);
    CHECK(rc == 0, "style_index: parse succeeds");
    CHECK(a.style_index == 3, "style_index: captured correctly");
}

static void test_style_index_and_outputpath(void)
{
    char* argv[] = { "doublespeak", "hi", "ctx", "pass", "2", "out.txt" };
    DoublespeakArgs a;
    int rc = doublespeak_parse_args(6, argv, &a);
    CHECK(rc == 0, "style+outputpath: parse succeeds");
    CHECK(a.style_index == 2, "style+outputpath: style_index captured");
    CHECK(a.output_path && strcmp(a.output_path, "out.txt") == 0,
          "style+outputpath: outputpath captured");
}

static void test_style_outputpath_and_url(void)
{
    char* argv[] = { "doublespeak", "hi", "ctx", "pass", "4", "out.txt", "http://host:1234" };
    DoublespeakArgs a;
    int rc = doublespeak_parse_args(7, argv, &a);
    CHECK(rc == 0, "style+outputpath+url: parse succeeds");
    CHECK(a.style_index == 4, "style+outputpath+url: style_index captured");
    CHECK(a.output_path && strcmp(a.output_path, "out.txt") == 0,
          "style+outputpath+url: outputpath captured");
    CHECK(a.llm_url && strcmp(a.llm_url, "http://host:1234") == 0,
          "style+outputpath+url: URL captured");
}

static void test_missing_context_and_passphrase(void)
{
    char* argv1[] = { "doublespeak", "hi" };
    DoublespeakArgs a;
    CHECK(doublespeak_parse_args(2, argv1, &a) != 0,
          "missing context+passphrase: parse fails");

    char* argv2[] = { "doublespeak", "hi", "ctx" };
    CHECK(doublespeak_parse_args(3, argv2, &a) != 0,
          "missing passphrase: parse fails");
}

static void test_invalid_style_index(void)
{
    DoublespeakArgs a;
    char* argv_zero[] = { "doublespeak", "hi", "ctx", "pass", "0" };
    CHECK(doublespeak_parse_args(5, argv_zero, &a) != 0,
          "style_index 0: parse fails (out of range)");

    char* argv_five[] = { "doublespeak", "hi", "ctx", "pass", "5" };
    CHECK(doublespeak_parse_args(5, argv_five, &a) != 0,
          "style_index 5: parse fails (out of range)");

    char* argv_word[] = { "doublespeak", "hi", "ctx", "pass", "abc" };
    CHECK(doublespeak_parse_args(5, argv_word, &a) != 0,
          "style_index 'abc': parse fails (not an integer)");
}

static void test_too_many_arguments(void)
{
    char* argv[] = { "doublespeak", "hi", "ctx", "pass", "1", "out.txt", "http://h", "extra" };
    DoublespeakArgs a;
    CHECK(doublespeak_parse_args(8, argv, &a) != 0,
          "extra trailing argument: parse fails");
}

static void test_no_arguments(void)
{
    char* argv[] = { "doublespeak" };
    DoublespeakArgs a;
    CHECK(doublespeak_parse_args(1, argv, &a) != 0,
          "no arguments: parse fails");
}

/* ── Group B: real encode/decode round trip (LLM-dependent) ─────────────── */

static int server_reachable(void)
{
    LLMClient* c = llm_client_create("http://127.0.0.1:8080", 6, 3000);
    if (!c) return 0;
    int ok = llm_client_health(c);
    llm_client_destroy(c);
    return ok;
}

static void test_roundtrip_direct_text(void)
{
    char* enc_argv[] = { "doublespeak", "h", "commute topic", "cli-test-pass" };
    DoublespeakArgs enc_args;
    CHECK(doublespeak_parse_args(4, enc_argv, &enc_args) == 0,
          "roundtrip(direct): parse encode args");

    /* Capture stdout isn't practical here, so exercise doublespeak_run()
       through a temp output file instead of the default stdout path. */
    enc_args.output_path = "doublespeak_cli_test_direct.covertext";
    int rc = doublespeak_run(&enc_args);
    CHECK(rc == METEOR_OK, "roundtrip(direct): encode succeeds");

    char* dec_argv[] = { "doublespeak", (char*)enc_args.output_path, "-f", "-d",
                          "commute topic", "cli-test-pass" };
    DoublespeakArgs dec_args;
    CHECK(doublespeak_parse_args(6, dec_argv, &dec_args) == 0,
          "roundtrip(direct): parse decode args");
    dec_args.output_path = "doublespeak_cli_test_direct.recovered";
    rc = doublespeak_run(&dec_args);
    CHECK(rc == METEOR_OK, "roundtrip(direct): decode succeeds");

    FILE* fp = fopen(dec_args.output_path, "rb");
    CHECK(fp != NULL, "roundtrip(direct): recovered file opens");
    if (fp) {
        char buf[64] = {0};
        size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
        fclose(fp);
        CHECK(n >= 1 && buf[0] == 'h', "roundtrip(direct): recovered message matches");
    }

    remove(enc_args.output_path);
    remove(dec_args.output_path);
}

static void test_roundtrip_file_message(void)
{
    char* enc_argv[] = { "doublespeak", METEOR_TEST_FIXTURES_DIR "/sample_message.txt",
                          "-f", "commute topic", "cli-test-pass" };
    DoublespeakArgs enc_args;
    CHECK(doublespeak_parse_args(5, enc_argv, &enc_args) == 0,
          "roundtrip(file): parse encode args");

    enc_args.output_path = "doublespeak_cli_test_file.covertext";
    int rc = doublespeak_run(&enc_args);
    CHECK(rc == METEOR_OK, "roundtrip(file): encode succeeds");

    char* dec_argv[] = { "doublespeak", (char*)enc_args.output_path, "-f", "-d",
                          "commute topic", "cli-test-pass" };
    DoublespeakArgs dec_args;
    CHECK(doublespeak_parse_args(6, dec_argv, &dec_args) == 0,
          "roundtrip(file): parse decode args");
    dec_args.output_path = "doublespeak_cli_test_file.recovered";
    rc = doublespeak_run(&dec_args);
    CHECK(rc == METEOR_OK, "roundtrip(file): decode succeeds");

    FILE* fp = fopen(dec_args.output_path, "rb");
    CHECK(fp != NULL, "roundtrip(file): recovered file opens");
    if (fp) {
        char buf[64] = {0};
        size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
        fclose(fp);
        CHECK(n >= 1 && buf[0] == 'h', "roundtrip(file): recovered message matches");
    }

    remove(enc_args.output_path);
    remove(dec_args.output_path);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    test_minimal();
    test_flag_f();
    test_flag_d();
    test_flags_f_then_d();
    test_flags_d_then_f();
    test_style_index();
    test_style_index_and_outputpath();
    test_style_outputpath_and_url();
    test_missing_context_and_passphrase();
    test_invalid_style_index();
    test_too_many_arguments();
    test_no_arguments();

    if (!server_reachable()) {
        printf("llama-server not reachable — skipping doublespeak roundtrip tests.\n");
    } else {
        printf("Server reachable — running doublespeak roundtrip tests...\n");
        test_roundtrip_direct_text();
        test_roundtrip_file_message();
    }

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
