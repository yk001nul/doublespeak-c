#pragma once

#include <stddef.h>

typedef struct {
    int  word_index;
    int  syl_index;
    int  syl_count;
    char syllable[64];
    int  is_last_in_word;
} SylToken;

/*
 * Opaque handle for the syllabifier.
 * Created once and reused; holds the libhyphen dictionary if available.
 */
typedef struct SylCtx SylCtx;

SylCtx* syllabifier_create(const char* hyphen_dict_path); /* NULL = heuristic only */
void    syllabifier_destroy(SylCtx* ctx);

/*
 * Split a single word into syllables.
 * Returns array of heap-allocated strings; *out_count is set.
 * Caller frees each string and the array itself.
 */
char** syllabify_word(SylCtx* ctx, const char* word, int* out_count);

/*
 * Syllabify all words in text.
 * Returns flat SylToken array; *out_count is set.
 * Caller frees with syllabify_text_free().
 */
SylToken* syllabify_text(SylCtx* ctx, const char* text, int* out_count);
void      syllabify_text_free(SylToken* tokens);

/* Free a syllable list returned by syllabify_word. */
void syllabify_word_free(char** syls, int count);
