#include "encode.h"
#include "meteor_ctx.h"
#include "prng.h"
#include "bits.h"
#include "meteor_core.h"
#include "llm_client.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Maximum syllable string length for joining a partial word. */
#define MAX_PARTIAL_LEN 512

char* meteor_encode_impl(struct MeteorCtx* ctx,
                         const uint8_t*    message,
                         size_t            msg_len,
                         const char*       starting_context,
                         int*              out_error)
{
    *out_error = METEOR_OK;

    /* convert message to bit array */
    size_t   total_bits;
    uint8_t* msg_bits = bits_from_bytes(message, msg_len, &total_bits);
    if (!msg_bits) { *out_error = METEOR_ERR_OOM; return NULL; }

    /* seed PRNG */
    MeteorPRNG prng;
    int rc = prng_init_raw(&prng, ctx->key);
    if (rc != 0) {
        free(msg_bits);
        *out_error = METEOR_ERR_CRYPTO;
        return NULL;
    }

    /* build style preamble (NULL in legacy mode) */
    char* preamble = llm_client_build_preamble((int)ctx->style, starting_context);

    /* allocate output buffer; in style mode the covertext starts empty */
    size_t ctx_len = (ctx->style == METEOR_STYLE_NONE && starting_context)
                     ? strlen(starting_context) : 0;
    size_t buf_cap  = ctx_len + (size_t)ctx->max_steps * 32 + 64;
    char*  full_text = (char*)malloc(buf_cap);
    if (!full_text) {
        free(preamble);
        free(msg_bits);
        prng_wipe(&prng);
        *out_error = METEOR_ERR_OOM;
        return NULL;
    }
    if (ctx_len > 0)
        memcpy(full_text, starting_context, ctx_len);
    full_text[ctx_len] = '\0';

    /* Seed the sentence with a fixed opener so the first LLM query is
       conditioned on a natural sentence start ("I", "Scientists", ...).
       The seed is included verbatim in the covertext; the decoder skips it. */
    if (ctx->style != METEOR_STYLE_NONE) {
        const char* seed = llm_client_style_seed((int)ctx->style);
        if (seed) {
            size_t slen = strlen(seed);
            if (slen + 1 > buf_cap) {
                buf_cap = slen + (size_t)ctx->max_steps * 32 + 64;
                char* tmp = (char*)realloc(full_text, buf_cap);
                if (!tmp) {
                    free(preamble); free(msg_bits); prng_wipe(&prng);
                    *out_error = METEOR_ERR_OOM;
                    return NULL;
                }
                full_text = tmp;
            }
            memcpy(full_text, seed, slen);
            full_text[slen] = '\0';
        }
    }

    size_t bit_offset = 0;
    int    steps      = 0;

    if (ctx->style != METEOR_STYLE_NONE) {
        /* Hard reject: even at theoretical maximum (beta bits/step × max_steps)
           the step budget cannot hold the message. */
        if ((size_t)(ctx->max_steps * ctx->beta) < total_bits) {
            free(preamble); free(msg_bits); free(full_text); prng_wipe(&prng);
            *out_error = METEOR_ERR_CAPACITY;
            return NULL;
        }

        /* ── Word-level encode loop (embellishment mode) ────────────────── */
        char last_word[64] = {0}; /* blacklist: word chosen in the previous step */
        while (bit_offset < total_bits && steps < ctx->max_steps) {
            LLMResponse* resp = llm_client_get_word_dist(
                ctx->llm, preamble, full_text,
                last_word[0] ? last_word : NULL);
            if (!resp) { *out_error = METEOR_ERR_LLM; break; }

            const char** w_texts = (const char**)malloc((size_t)resp->count * sizeof(char*));
            float*       w_probs = (float*)malloc((size_t)resp->count * sizeof(float));
            if (!w_texts || !w_probs) {
                free(w_texts); free(w_probs); llm_response_free(resp);
                *out_error = METEOR_ERR_OOM; break;
            }
            for (int i = 0; i < resp->count; i++) {
                w_texts[i] = resp->candidates[i].text;
                w_probs[i] = resp->candidates[i].prob;
            }
            MeteorDist* dist = meteor_build_dist(w_texts, w_probs, resp->count, ctx->beta);
            free(w_texts); free(w_probs); llm_response_free(resp);
            if (!dist) { *out_error = METEOR_ERR_OOM; break; }

            MeteorStepResult step = meteor_encode_step(
                dist, msg_bits, bit_offset, total_bits, &prng, ctx->beta);
            meteor_free_dist(dist);

            bit_offset += (size_t)step.cp_len;
            steps++;

            strncpy(last_word, step.chosen, 63);
            last_word[63] = '\0';

            /* append chosen word to covertext */
            size_t wlen   = strlen(step.chosen);
            size_t ft_len = strlen(full_text);
            size_t needed = ft_len + 1 + wlen + 2;
            if (needed > buf_cap) {
                buf_cap = needed * 2;
                char* tmp = (char*)realloc(full_text, buf_cap);
                if (!tmp) { *out_error = METEOR_ERR_OOM; free(full_text); full_text = NULL; break; }
                full_text = tmp;
            }
            if (ft_len > 0) full_text[ft_len++] = ' ';
            memcpy(full_text + ft_len, step.chosen, wlen);
            full_text[ft_len + wlen] = '\0';
        }
    } else {
        /* ── Syllable-level encode loop (legacy mode) ───────────────────── */
        char partial_word[MAX_PARTIAL_LEN] = {0};

        while ((bit_offset < total_bits || partial_word[0] != '\0') && steps < ctx->max_steps) {
            int is_new_word = (partial_word[0] == '\0');

            LLMResponse* resp = llm_client_get_syllable_dist(
                ctx->llm, preamble, full_text, partial_word, is_new_word);
            if (!resp) { *out_error = METEOR_ERR_LLM; break; }

            const char** syl_texts = (const char**)malloc((size_t)resp->count * sizeof(char*));
            float*       syl_probs = (float*)malloc((size_t)resp->count * sizeof(float));
            if (!syl_texts || !syl_probs) {
                free(syl_texts); free(syl_probs); llm_response_free(resp);
                *out_error = METEOR_ERR_OOM; break;
            }
            for (int i = 0; i < resp->count; i++) {
                syl_texts[i] = resp->candidates[i].text;
                syl_probs[i] = resp->candidates[i].prob;
            }
            MeteorDist* dist = meteor_build_dist(syl_texts, syl_probs, resp->count, ctx->beta);
            free(syl_texts); free(syl_probs); llm_response_free(resp);
            if (!dist) { *out_error = METEOR_ERR_OOM; break; }

            MeteorStepResult step = meteor_encode_step(
                dist, msg_bits, bit_offset, total_bits, &prng, ctx->beta);
            meteor_free_dist(dist);

            bit_offset += (size_t)step.cp_len;
            steps++;

            if (strcmp(step.chosen, EOW_TOKEN) == 0) {
                size_t pw_len = strlen(partial_word);
                size_t ft_len = strlen(full_text);
                size_t needed = ft_len + 1 + pw_len + 2;
                if (needed > buf_cap) {
                    buf_cap = needed * 2;
                    char* tmp = (char*)realloc(full_text, buf_cap);
                    if (!tmp) { *out_error = METEOR_ERR_OOM; free(full_text); full_text = NULL; break; }
                    full_text = tmp;
                }
                if (ft_len > 0) full_text[ft_len++] = ' ';
                memcpy(full_text + ft_len, partial_word, pw_len);
                full_text[ft_len + pw_len] = '\0';
                partial_word[0] = '\0';
            } else {
                strncat(partial_word, step.chosen, MAX_PARTIAL_LEN - strlen(partial_word) - 1);
            }
        }

        /* finalise any dangling partial word */
        if (partial_word[0] != '\0' && full_text) {
            size_t pw_len = strlen(partial_word);
            size_t ft_len = strlen(full_text);
            size_t needed = ft_len + 1 + pw_len + 2;
            if (needed > buf_cap) {
                char* tmp = (char*)realloc(full_text, needed);
                if (!tmp) { *out_error = METEOR_ERR_OOM; free(full_text); full_text = NULL; }
                else full_text = tmp;
            }
            if (full_text) {
                if (ft_len > 0) full_text[ft_len++] = ' ';
                memcpy(full_text + ft_len, partial_word, pw_len);
                full_text[ft_len + pw_len] = '\0';
            }
        }
    }

    free(preamble);
    free(msg_bits);
    prng_wipe(&prng);

    if (!full_text) return NULL;
    if (*out_error != METEOR_OK && *out_error != METEOR_ERR_LLM) {
        free(full_text);
        return NULL;
    }
    return full_text;
}
