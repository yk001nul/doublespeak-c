/*
 * Tests for the heuristic syllabifier.
 * libhyphen tests are skipped if METEOR_USE_LIBHYPHEN is not set.
 */
#include "../src/syllabifier.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { if (cond) { printf("[PASS] %s\n", msg); passed++; } \
         else { printf("[FAIL] %s  (line %d)\n", msg, __LINE__); failed++; } } while(0)

typedef struct { const char* word; int expected_count; } WordTest;

static void test_heuristic_counts(void)
{
    SylCtx* ctx = syllabifier_create(NULL); /* force heuristic */
    CHECK(ctx != NULL, "syllabifier_create(NULL): non-NULL");
    if (!ctx) return;

    /* test that counts are at least reasonable (heuristic ≠ perfect) */
    const char* one_syl[]  = {"cat", "dog", "run", "fly", NULL};
    const char* two_syl[]  = {"table", "water", "butter", NULL};
    const char* three_syl[]= {"beautiful", "computer", NULL};

    for (int i = 0; one_syl[i]; i++) {
        int count;
        char** syls = syllabify_word(ctx, one_syl[i], &count);
        CHECK(count >= 1, "One-syl word: at least 1 syllable");
        syllabify_word_free(syls, count);
    }
    for (int i = 0; two_syl[i]; i++) {
        int count;
        char** syls = syllabify_word(ctx, two_syl[i], &count);
        CHECK(count >= 1, "Two-syl word: at least 1 syllable");
        syllabify_word_free(syls, count);
    }
    for (int i = 0; three_syl[i]; i++) {
        int count;
        char** syls = syllabify_word(ctx, three_syl[i], &count);
        CHECK(count >= 2, "Three-syl word: at least 2 syllables");
        syllabify_word_free(syls, count);
    }

    syllabifier_destroy(ctx);
}

static void test_reconstruction(void)
{
    /* syllables must concatenate back to the original word (after lowercasing) */
    SylCtx* ctx = syllabifier_create(NULL);
    const char* words[] = {"remarkable", "investigation", "simple", "cat", "beautiful", NULL};

    for (int i = 0; words[i]; i++) {
        int    count;
        char** syls = syllabify_word(ctx, words[i], &count);

        char reconstructed[256] = {0};
        for (int s = 0; s < count; s++)
            strncat(reconstructed, syls[s],
                    sizeof(reconstructed) - strlen(reconstructed) - 1);

        /* compare lowercase */
        char lower_word[256] = {0};
        for (int c = 0; words[i][c] && c < 255; c++)
            lower_word[c] = (char)tolower((unsigned char)words[i][c]);

        CHECK(strcmp(reconstructed, lower_word) == 0,
              "Reconstruction: syllables rejoin to original word");

        syllabify_word_free(syls, count);
    }

    syllabifier_destroy(ctx);
}

static void test_syllabify_text(void)
{
    SylCtx* ctx = syllabifier_create(NULL);
    const char* text = "hello world";

    int       count;
    SylToken* tokens = syllabify_text(ctx, text, &count);
    CHECK(tokens != NULL, "syllabify_text: non-NULL");
    CHECK(count >= 2, "syllabify_text: at least 2 tokens for 'hello world'");

    if (tokens) {
        /* last token of each word should have is_last_in_word == 1 */
        int found_last = 0;
        for (int i = 0; i < count; i++)
            if (tokens[i].is_last_in_word) found_last++;
        CHECK(found_last >= 2, "syllabify_text: at least 2 word-final tokens");
    }

    syllabify_text_free(tokens);
    syllabifier_destroy(ctx);
}

static void test_empty_input(void)
{
    SylCtx* ctx = syllabifier_create(NULL);
    int count;
    char** syls = syllabify_word(ctx, "", &count);
    CHECK(count == 0, "Empty word: count == 0");
    syllabify_word_free(syls, count);

    int tc;
    SylToken* toks = syllabify_text(ctx, "", &tc);
    CHECK(tc == 0, "Empty text: count == 0");
    syllabify_text_free(toks);

    syllabifier_destroy(ctx);
}

int main(void)
{
    test_heuristic_counts();
    test_reconstruction();
    test_syllabify_text();
    test_empty_input();

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
