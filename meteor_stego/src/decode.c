#include "decode.h"
#include "meteor_ctx.h"
#include "prng.h"
#include "bits.h"
#include "meteor_core.h"
#include "llm_client.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_RECOVERED_BITS 8192

static int null_terminator_found(const uint8_t* bits, size_t count)
{
    if (count < 8) return 0;
    for (size_t i = count - 8; i < count; i++)
        if (bits[i] != 0) return 0;
    return 1;
}

/* Count space-separated words in a string. */
static int count_words(const char* s)
{
    if (!s) return 0;
    int n = 0, in_word = 0;
    while (*s) {
        if (isalpha((unsigned char)*s)) { if (!in_word) { n++; in_word = 1; } }
        else                            { in_word = 0; }
        s++;
    }
    return n;
}

/*
 * Extract lowercase alpha-only words from text, skipping the first skip_count
 * words.  Returns a NULL-terminated array of heap strings; caller frees with
 * free_words().
 */
static char** extract_words(const char* text, int skip_count, int* out_count)
{
    *out_count = 0;
    if (!text || !*text) return NULL;

    /* first pass: count total words */
    int total = count_words(text);
    int n = total - skip_count;
    if (n <= 0) return NULL;

    char** words = (char**)calloc((size_t)(n + 1), sizeof(char*));
    if (!words) return NULL;

    const char* p = text;
    int wi = 0, ri = 0;
    while (*p && ri < n) {
        while (*p && !isalpha((unsigned char)*p)) p++;
        if (!*p) break;
        const char* ws = p;
        while (*p && isalpha((unsigned char)*p)) p++;
        if (wi >= skip_count) {
            size_t wlen = (size_t)(p - ws);
            char*  w    = (char*)malloc(wlen + 1);
            if (!w) { for (int i = 0; i < ri; i++) free(words[i]); free(words); return NULL; }
            for (size_t i = 0; i < wlen; i++)
                w[i] = (char)tolower((unsigned char)ws[i]);
            w[wlen] = '\0';
            words[ri++] = w;
        }
        wi++;
    }
    *out_count = ri;
    return words;
}

static void free_words(char** words, int count)
{
    if (!words) return;
    for (int i = 0; i < count; i++) free(words[i]);
    free(words);
}

/* Run one decode step; returns bits recovered. Frees dist, resp, syl arrays. */
static int run_decode_step(const char*  chosen_text,
                           MeteorDist*  dist,
                           MeteorPRNG*  prng,
                           int          beta,
                           uint8_t*     out_bits)
{
    int n = meteor_decode_step(chosen_text, dist, prng, beta, out_bits);
    meteor_free_dist(dist);
    return n;
}

uint8_t* meteor_decode_impl(struct MeteorCtx* ctx,
                             const char*       covertext,
                             const char*       starting_context,
                             size_t*           out_msg_len,
                             int*              out_error)
{
    *out_error   = METEOR_OK;
    *out_msg_len = 0;

    /* build style preamble (NULL in legacy mode) */
    char* preamble = llm_client_build_preamble((int)ctx->style, starting_context);

    /* words[] is only populated in the syllable-mode path; NULL-safe free at end */
    int    word_count = 0;
    char** words      = NULL;

    MeteorPRNG prng;
    int rc = prng_init_raw(&prng, ctx->key);
    if (rc != 0) {
        free(preamble);
        *out_error = METEOR_ERR_CRYPTO;
        return NULL;
    }

    uint8_t* recovered_bits = (uint8_t*)calloc(MAX_RECOVERED_BITS, 1);
    size_t   rb_count       = 0;

    size_t recon_cap  = 4096;
    char*  full_recon = (char*)malloc(recon_cap);
    if (!full_recon || !recovered_bits) {
        free(recovered_bits); free(full_recon);
        free(preamble);
        prng_wipe(&prng);
        *out_error = METEOR_ERR_OOM;
        return NULL;
    }
    size_t recon_len = 0;
    {
        const char* recon_seed = (ctx->style == METEOR_STYLE_NONE)
                                 ? starting_context : NULL;
        if (recon_seed && recon_seed[0]) {
            recon_len = strlen(recon_seed);
            if (recon_len + 1 > recon_cap) {
                recon_cap  = recon_len + 4096;
                full_recon = (char*)realloc(full_recon, recon_cap);
                if (!full_recon) {
                    free(recovered_bits); free(preamble); prng_wipe(&prng);
                    *out_error = METEOR_ERR_OOM; return NULL;
                }
            }
            memcpy(full_recon, recon_seed, recon_len);
        }
    }
    full_recon[recon_len] = '\0';

    int done = 0;

    if (ctx->style != METEOR_STYLE_NONE) {
        /* ── Phrase-level decode loop (style mode) ────────────────────── */
        const char* remaining = covertext;
        while (*remaining == ' ') remaining++;

        /* Must mirror encode.c's history depth/join format exactly — the
           blacklist text is part of the LLM prompt and any divergence
           corrupts prefix-matching for every following step. */
        #define PHRASE_HISTORY 6
        char phrase_history[PHRASE_HISTORY][64] = {{0}};
        int  hist_count = 0;
        int  steps      = 0;
        char subject_anchor[64] = {0};
        MeteorWordHistory content_hist = {0};

        while (*remaining && !done && steps < ctx->max_steps) {
            char blacklist_buf[512] = {0};
            size_t bl_off = 0;
            for (int i = 0; i < hist_count; i++) {
                int w = snprintf(blacklist_buf + bl_off, sizeof(blacklist_buf) - bl_off,
                                  "%s%s", i > 0 ? ", " : "", phrase_history[i]);
                if (w > 0) bl_off += (size_t)w;
            }
            char word_blacklist_buf[256] = {0};
            meteor_word_history_join(&content_hist, word_blacklist_buf,
                                      sizeof(word_blacklist_buf));

            LLMResponse* resp = llm_client_get_phrase_dist(
                ctx->llm, preamble, full_recon,
                hist_count > 0 ? blacklist_buf : NULL,
                word_blacklist_buf[0] ? word_blacklist_buf : NULL,
                subject_anchor[0] ? subject_anchor : NULL);
            if (!resp) { *out_error = METEOR_ERR_LLM; done = 1; break; }

            const char** p_texts = (const char**)malloc(
                (size_t)resp->count * sizeof(char*));
            float* p_probs = (float*)malloc(
                (size_t)resp->count * sizeof(float));
            if (!p_texts || !p_probs) {
                free(p_texts); free(p_probs); llm_response_free(resp);
                *out_error = METEOR_ERR_OOM; done = 1; break;
            }
            for (int i = 0; i < resp->count; i++) {
                p_texts[i] = resp->candidates[i].text;
                p_probs[i] = resp->candidates[i].prob;
            }
            MeteorDist* dist = meteor_build_dist(
                p_texts, p_probs, resp->count, ctx->beta);
            free(p_texts); free(p_probs); llm_response_free(resp);
            if (!dist) { *out_error = METEOR_ERR_OOM; done = 1; break; }

            /* find the longest candidate that is a prefix of remaining */
            const char* chosen     = NULL;
            int         chosen_len = 0;
            for (int i = 0; i < dist->count; i++) {
                const char* cand = dist->slots[i].text;
                int clen = (int)strlen(cand);
                if (clen > chosen_len &&
                    strncmp(remaining, cand, (size_t)clen) == 0) {
                    chosen     = cand;
                    chosen_len = clen;
                }
            }
            if (!chosen) {
                chosen     = dist->slots[0].text;
                chosen_len = (int)strlen(chosen);
            }

            char chosen_buf[64];
            strncpy(chosen_buf, chosen, 63);
            chosen_buf[63] = '\0';
            int advance = chosen_len;

            uint8_t step_bits[32];
            int nbits = run_decode_step(chosen_buf, dist, &prng, ctx->beta, step_bits);

            for (int b = 0; b < nbits && rb_count < MAX_RECOVERED_BITS; b++)
                recovered_bits[rb_count++] = step_bits[b];

            if (null_terminator_found(recovered_bits, rb_count)) { done = 1; break; }

            if (subject_anchor[0] == '\0') {
                strncpy(subject_anchor, chosen_buf, 63);
                subject_anchor[63] = '\0';
            }

            meteor_word_history_add(&content_hist, chosen_buf);

            if (hist_count < PHRASE_HISTORY) {
                strncpy(phrase_history[hist_count], chosen_buf, 63);
                phrase_history[hist_count][63] = '\0';
                hist_count++;
            } else {
                for (int i = 0; i < PHRASE_HISTORY - 1; i++)
                    memcpy(phrase_history[i], phrase_history[i + 1], 64);
                strncpy(phrase_history[PHRASE_HISTORY - 1], chosen_buf, 63);
                phrase_history[PHRASE_HISTORY - 1][63] = '\0';
            }

            /* advance past the matched phrase and any following space */
            remaining += advance;
            while (*remaining == ' ') remaining++;

            /* advance full_recon */
            size_t plen   = strlen(chosen_buf);
            size_t needed = recon_len + 1 + plen + 2;
            if (needed > recon_cap) {
                recon_cap  = needed * 2;
                full_recon = (char*)realloc(full_recon, recon_cap);
                if (!full_recon) { *out_error = METEOR_ERR_OOM; done = 1; break; }
            }
            if (recon_len > 0) full_recon[recon_len++] = ' ';
            memcpy(full_recon + recon_len, chosen_buf, plen);
            recon_len += plen;
            full_recon[recon_len] = '\0';

            steps++;
        }
        #undef PHRASE_HISTORY
    } else {
        /* ── Syllable-level decode loop (legacy mode) ─────────────────── */
        int skip_words = count_words(starting_context);
        words = extract_words(covertext, skip_words, &word_count);

        for (int wi = 0; wi < word_count && !done; wi++) {
            const char* word      = words[wi];
            const char* remaining = word;
            char        partial[512] = {0};
            int         is_new = 1;

            while (*remaining && !done) {
                LLMResponse* resp = llm_client_get_syllable_dist(
                    ctx->llm, preamble, full_recon, partial, is_new);
                if (!resp) { *out_error = METEOR_ERR_LLM; done = 1; break; }

                const char** syl_texts = (const char**)malloc(
                    (size_t)resp->count * sizeof(char*));
                float* syl_probs = (float*)malloc(
                    (size_t)resp->count * sizeof(float));
                if (!syl_texts || !syl_probs) {
                    free(syl_texts); free(syl_probs); llm_response_free(resp);
                    *out_error = METEOR_ERR_OOM; done = 1; break;
                }
                for (int i = 0; i < resp->count; i++) {
                    syl_texts[i] = resp->candidates[i].text;
                    syl_probs[i] = resp->candidates[i].prob;
                }
                MeteorDist* dist = meteor_build_dist(
                    syl_texts, syl_probs, resp->count, ctx->beta);
                free(syl_texts); free(syl_probs); llm_response_free(resp);
                if (!dist) { *out_error = METEOR_ERR_OOM; done = 1; break; }

                const char* chosen     = NULL;
                int         chosen_len = 0;
                for (int i = 0; i < dist->count; i++) {
                    const char* cand = dist->slots[i].text;
                    if (cand[0] == '\x01') continue;
                    int clen = (int)strlen(cand);
                    if (clen > chosen_len &&
                        strncmp(remaining, cand, (size_t)clen) == 0) {
                        chosen     = cand;
                        chosen_len = clen;
                    }
                }
                if (!chosen) {
                    for (int i = 0; i < dist->count; i++) {
                        if (dist->slots[i].text[0] != '\x01') {
                            chosen     = dist->slots[i].text;
                            chosen_len = (int)strlen(remaining);
                            break;
                        }
                    }
                    if (!chosen) { meteor_free_dist(dist); done = 1; break; }
                }

                char chosen_buf[64];
                strncpy(chosen_buf, chosen, 63); chosen_buf[63] = '\0';
                int chosen_len_copy = chosen_len;

                uint8_t step_bits[32];
                int nbits = run_decode_step(chosen_buf, dist, &prng, ctx->beta, step_bits);

                for (int b = 0; b < nbits && rb_count < MAX_RECOVERED_BITS; b++)
                    recovered_bits[rb_count++] = step_bits[b];

                if (null_terminator_found(recovered_bits, rb_count)) { done = 1; break; }

                strncat(partial, chosen_buf, sizeof(partial) - strlen(partial) - 1);
                remaining += chosen_len_copy;
                is_new = 0;
            }

            if (done) break;

            /* synthesise EOW step */
            LLMResponse* eow_resp = llm_client_get_syllable_dist(
                ctx->llm, preamble, full_recon, partial, 0);
            if (eow_resp) {
                const char** et = (const char**)malloc(
                    (size_t)eow_resp->count * sizeof(char*));
                float* ep = (float*)malloc(
                    (size_t)eow_resp->count * sizeof(float));
                if (et && ep) {
                    for (int i = 0; i < eow_resp->count; i++) {
                        et[i] = eow_resp->candidates[i].text;
                        ep[i] = eow_resp->candidates[i].prob;
                    }
                    MeteorDist* eow_dist = meteor_build_dist(
                        et, ep, eow_resp->count, ctx->beta);
                    if (eow_dist) {
                        uint8_t eow_bits[32];
                        int en = run_decode_step(
                            EOW_TOKEN, eow_dist, &prng, ctx->beta, eow_bits);
                        for (int b = 0; b < en && rb_count < MAX_RECOVERED_BITS; b++)
                            recovered_bits[rb_count++] = eow_bits[b];
                    }
                }
                free(et); free(ep);
                llm_response_free(eow_resp);
            }

            if (null_terminator_found(recovered_bits, rb_count)) { done = 1; break; }

            size_t wlen   = strlen(word);
            size_t needed = recon_len + 1 + wlen + 2;
            if (needed > recon_cap) {
                recon_cap  = needed * 2;
                full_recon = (char*)realloc(full_recon, recon_cap);
                if (!full_recon) { *out_error = METEOR_ERR_OOM; done = 1; break; }
            }
            if (recon_len > 0) full_recon[recon_len++] = ' ';
            memcpy(full_recon + recon_len, word, wlen);
            recon_len += wlen;
            full_recon[recon_len] = '\0';
        }
    }

    free_words(words, word_count);
    prng_wipe(&prng);
    free(full_recon);
    free(preamble);

    size_t   msg_len;
    uint8_t* msg = bits_to_bytes(recovered_bits, rb_count, &msg_len);
    free(recovered_bits);

    *out_msg_len = msg_len;
    return msg;
}
