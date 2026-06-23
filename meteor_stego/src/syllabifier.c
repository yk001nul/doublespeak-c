#include "syllabifier.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

#ifdef METEOR_USE_LIBHYPHEN
#  include <hyphen.h>
#endif

struct SylCtx {
#ifdef METEOR_USE_LIBHYPHEN
    HyphenDict* dict;
#else
    int         unused;
#endif
    int use_libhyphen;
};

/* ── lifecycle ────────────────────────────────────────────────────────────── */

SylCtx* syllabifier_create(const char* hyphen_dict_path)
{
    SylCtx* ctx = (SylCtx*)calloc(1, sizeof(SylCtx));
    if (!ctx) return NULL;

#ifdef METEOR_USE_LIBHYPHEN
    if (hyphen_dict_path) {
        ctx->dict = hnj_hyphen_load(hyphen_dict_path);
        if (ctx->dict) {
            ctx->use_libhyphen = 1;
            return ctx;
        }
        fprintf(stderr, "meteor: failed to load hyphen dictionary '%s'; "
                        "falling back to heuristic.\n", hyphen_dict_path);
    }
#else
    (void)hyphen_dict_path;
#endif

    ctx->use_libhyphen = 0;
    return ctx;
}

void syllabifier_destroy(SylCtx* ctx)
{
    if (!ctx) return;
#ifdef METEOR_USE_LIBHYPHEN
    if (ctx->dict) hnj_hyphen_free(ctx->dict);
#endif
    free(ctx);
}

/* ── helpers ──────────────────────────────────────────────────────────────── */

static int is_vowel(char c)
{
    c = (char)tolower((unsigned char)c);
    return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y';
}

/* Known two-letter digraphs that must not be split. */
static int is_digraph(char a, char b)
{
    static const char* DIGRAPHS[] = {
        "ch","sh","th","wh","ph","ck","ng","qu","tr","br","cr","dr",
        "fr","gr","pr","bl","cl","fl","gl","pl","sl","sp","st","sc",
        "sk","sm","sn","sw","tw","dw",
        NULL
    };
    char pair[3] = { (char)tolower((unsigned char)a), (char)tolower((unsigned char)b), 0 };
    for (int i = 0; DIGRAPHS[i]; i++)
        if (strcmp(pair, DIGRAPHS[i]) == 0) return 1;
    return 0;
}

/* Returns a newly allocated copy of word[start..end) */
static char* substr(const char* word, int start, int end)
{
    int   len = end - start;
    char* s   = (char*)malloc((size_t)(len + 1));
    if (!s) return NULL;
    memcpy(s, word + start, (size_t)len);
    s[len] = '\0';
    return s;
}

/*
 * Heuristic syllabifier based on vowel-cluster and VC|CV rules.
 * Accuracy ~80–85% on common English.
 */
static char** syllabify_heuristic(const char* word, int* out_count)
{
    int len = (int)strlen(word);
    if (len == 0) {
        *out_count = 0;
        return NULL;
    }

    /* strip to lowercase alpha only */
    char* clean = (char*)malloc((size_t)(len + 1));
    if (!clean) { *out_count = 0; return NULL; }
    int clen = 0;
    for (int i = 0; i < len; i++) {
        if (isalpha((unsigned char)word[i]))
            clean[clen++] = (char)tolower((unsigned char)word[i]);
    }
    clean[clen] = '\0';

    if (clen == 0) {
        free(clean);
        *out_count = 0;
        return NULL;
    }

    /* locate vowel nuclei */
    int* v_pos = (int*)calloc((size_t)clen, sizeof(int));
    int  v_count = 0;
    for (int i = 0; i < clen; i++)
        if (is_vowel(clean[i])) v_pos[v_count++] = i;

    if (v_count == 0) {
        /* no vowels — return whole word as single syllable */
        char** result = (char**)malloc(sizeof(char*));
        result[0] = substr(clean, 0, clen);
        free(v_pos); free(clean);
        *out_count = 1;
        return result;
    }

    /* build break positions (index in clean[] before which we break) */
    int* breaks  = (int*)calloc((size_t)(v_count + 1), sizeof(int));
    int  n_break = 0;

    for (int vi = 0; vi + 1 < v_count; vi++) {
        /* end of current nucleus (run of vowels) */
        int nuc_end = v_pos[vi];
        while (nuc_end + 1 < v_pos[vi + 1] && is_vowel(clean[nuc_end + 1]))
            nuc_end++;

        /* start of next nucleus */
        int next_nuc = v_pos[vi + 1];
        while (next_nuc > 0 && is_vowel(clean[next_nuc - 1]))
            next_nuc--;
        /* next_nuc now points to the consonant start after current nucleus */

        int consonants = next_nuc - nuc_end - 1; /* consonants between nuclei */

        int break_pos;
        if (consonants == 0) {
            /* two adjacent vowel groups — no break */
            continue;
        } else if (consonants == 1) {
            /* V|CV */
            break_pos = nuc_end + 1;
        } else {
            /* VC|CV or split with digraph awareness */
            int mid = nuc_end + 1 + consonants / 2;
            /* avoid splitting a digraph */
            if (mid > 0 && mid < clen &&
                is_digraph(clean[mid - 1], clean[mid]))
                mid++;
            break_pos = mid;
        }
        /* clamp */
        if (break_pos > 0 && break_pos < clen)
            breaks[n_break++] = break_pos;
    }

    /* deduplicate/sort breaks */
    for (int i = 0; i < n_break - 1; i++)
        for (int j = i + 1; j < n_break; j++)
            if (breaks[i] > breaks[j]) { int t = breaks[i]; breaks[i] = breaks[j]; breaks[j] = t; }

    /* build syllable array */
    int n_syl = n_break + 1;
    char** result = (char**)malloc((size_t)n_syl * sizeof(char*));
    if (!result) { free(breaks); free(v_pos); free(clean); *out_count = 0; return NULL; }

    int prev = 0;
    for (int i = 0; i < n_break; i++) {
        result[i] = substr(clean, prev, breaks[i]);
        prev = breaks[i];
    }
    result[n_break] = substr(clean, prev, clen);

    free(breaks); free(v_pos); free(clean);
    *out_count = n_syl;
    return result;
}

/* ── libhyphen implementation ─────────────────────────────────────────────── */

#ifdef METEOR_USE_LIBHYPHEN
static char** syllabify_libhyphen(HyphenDict* dict, const char* word, int* out_count)
{
    char*  hyphenated = NULL;
    char** rep        = NULL;
    int*   pos        = NULL;
    int*   cut        = NULL;
    int    len        = (int)strlen(word);

    hnj_hyphen_hyphenate2(dict, word, len, &hyphenated, NULL, &rep, &pos, &cut);

    /* count syllables (count '-' separators + 1) */
    int n = 1;
    if (hyphenated) {
        for (char* p = hyphenated; *p; p++)
            if (*p == '-') n++;
    }

    char** result = (char**)malloc((size_t)n * sizeof(char*));
    if (!result) {
        free(hyphenated);
        if (rep) { for (int i = 0; i < len; i++) free(rep[i]); free(rep); }
        free(pos); free(cut);
        *out_count = 0;
        return NULL;
    }

    int idx = 0, start = 0;
    for (int i = 0; hyphenated && hyphenated[i]; i++) {
        if (hyphenated[i] == '-') {
            result[idx++] = substr(hyphenated, start, i);
            start = i + 1;
        }
    }
    /* last segment */
    if (hyphenated)
        result[idx] = substr(hyphenated, start, (int)strlen(hyphenated));

    free(hyphenated);
    if (rep) { for (int i = 0; i < len; i++) free(rep[i]); free(rep); }
    free(pos); free(cut);

    *out_count = n;
    return result;
}
#endif

/* ── public API ───────────────────────────────────────────────────────────── */

char** syllabify_word(SylCtx* ctx, const char* word, int* out_count)
{
#ifdef METEOR_USE_LIBHYPHEN
    if (ctx && ctx->use_libhyphen && ctx->dict)
        return syllabify_libhyphen(ctx->dict, word, out_count);
#else
    (void)ctx;
#endif
    return syllabify_heuristic(word, out_count);
}

void syllabify_word_free(char** syls, int count)
{
    if (!syls) return;
    for (int i = 0; i < count; i++) free(syls[i]);
    free(syls);
}

SylToken* syllabify_text(SylCtx* ctx, const char* text, int* out_count)
{
    if (!text || *text == '\0') {
        if (out_count) *out_count = 0;
        return NULL;
    }

    /* tokenise text into words (split on non-alpha) */
    char* copy = strdup(text);
    if (!copy) { if (out_count) *out_count = 0; return NULL; }

    /* first pass: count tokens */
    SylToken* tokens = NULL;
    int       total  = 0;
    int       cap    = 0;

    int word_index = 0;
    char* p = copy;
    while (*p) {
        /* skip non-alpha */
        while (*p && !isalpha((unsigned char)*p)) p++;
        if (!*p) break;

        /* collect word */
        char* wstart = p;
        while (*p && isalpha((unsigned char)*p)) p++;
        char saved = *p;
        *p = '\0';

        int    syl_count;
        char** syls = syllabify_word(ctx, wstart, &syl_count);
        *p = saved;

        for (int si = 0; si < syl_count; si++) {
            if (total >= cap) {
                cap = cap ? cap * 2 : 64;
                tokens = (SylToken*)realloc(tokens, (size_t)cap * sizeof(SylToken));
                if (!tokens) { free(copy); if (out_count) *out_count = 0; return NULL; }
            }
            SylToken* t = &tokens[total++];
            t->word_index     = word_index;
            t->syl_index      = si;
            t->syl_count      = syl_count;
            t->is_last_in_word = (si == syl_count - 1) ? 1 : 0;
            strncpy(t->syllable, syls ? syls[si] : "", sizeof(t->syllable) - 1);
            t->syllable[sizeof(t->syllable) - 1] = '\0';
        }

        if (syls) syllabify_word_free(syls, syl_count);
        word_index++;
    }

    free(copy);
    if (out_count) *out_count = total;
    return tokens;
}

void syllabify_text_free(SylToken* tokens)
{
    free(tokens);
}
