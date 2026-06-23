#include "decode.h"
#include "meteor_ctx.h"
#include "prng.h"
#include "bits.h"
#include "meteor_core.h"
#include "llm_client.h"
#include "syllabifier.h"

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

uint8_t* meteor_decode_impl(struct MeteorCtx* ctx,
                             const char*       covertext,
                             const char*       starting_context,
                             size_t*           out_msg_len,
                             int*              out_error)
{
    *out_error  = METEOR_OK;
    *out_msg_len = 0;

    /* syllabify covertext */
    int       syl_count = 0;
    SylToken* tokens = syllabify_text(ctx->syl, covertext, &syl_count);

    /* skip starting_context words — count words in it */
    int skip_words = 0;
    if (starting_context && *starting_context) {
        const char* p = starting_context;
        int in_word = 0;
        while (*p) {
            if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) {
                if (!in_word) { skip_words++; in_word = 1; }
            } else {
                in_word = 0;
            }
            p++;
        }
    }

    /* determine token start index (after skip_words) */
    int start_token = 0;
    if (skip_words > 0 && tokens) {
        for (int i = 0; i < syl_count; i++) {
            if (tokens[i].word_index >= skip_words) {
                start_token = i;
                break;
            }
        }
    }

    /* seed PRNG (same seed as encoder) */
    MeteorPRNG prng;
    int rc = prng_init_raw(&prng, ctx->key);
    if (rc != 0) {
        syllabify_text_free(tokens);
        *out_error = METEOR_ERR_CRYPTO;
        return NULL;
    }

    uint8_t* recovered_bits = (uint8_t*)calloc(MAX_RECOVERED_BITS, 1);
    size_t   rb_count       = 0;

    /* reconstruct full_text starting from starting_context */
    size_t recon_cap  = 4096;
    char*  full_recon = (char*)malloc(recon_cap);
    if (!full_recon || !recovered_bits) {
        free(recovered_bits); free(full_recon);
        syllabify_text_free(tokens);
        prng_wipe(&prng);
        *out_error = METEOR_ERR_OOM;
        return NULL;
    }
    size_t recon_len = 0;
    if (starting_context) {
        recon_len = strlen(starting_context);
        if (recon_len + 1 > recon_cap) {
            recon_cap = recon_len + 4096;
            full_recon = (char*)realloc(full_recon, recon_cap);
        }
        memcpy(full_recon, starting_context, recon_len);
    }
    full_recon[recon_len] = '\0';

    char partial_word[512] = {0};
    int  done = 0;

    for (int ti = start_token; ti < syl_count && !done; ti++) {
        SylToken* t = &tokens[ti];
        int is_new = (t->syl_index == 0);

        /* get syllable distribution */
        LLMResponse* resp = llm_client_get_syllable_dist(
            ctx->llm, full_recon, partial_word, is_new);
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
        free(syl_texts); free(syl_probs);
        llm_response_free(resp);
        if (!dist) { *out_error = METEOR_ERR_OOM; break; }

        uint8_t step_bits[32];
        int     nbits = meteor_decode_step(t->syllable, dist, &prng, ctx->beta, step_bits);
        meteor_free_dist(dist);

        for (int b = 0; b < nbits && rb_count < MAX_RECOVERED_BITS; b++)
            recovered_bits[rb_count++] = step_bits[b];

        if (null_terminator_found(recovered_bits, rb_count)) { done = 1; break; }

        if (t->is_last_in_word) {
            /* synthesise the EOW step — always present in the encode sequence */
            char partial_plus_syl[512] = {0};
            if (partial_word[0] != '\0') {
                strncpy(partial_plus_syl, partial_word, 510);
                strncat(partial_plus_syl, t->syllable, 510 - strlen(partial_plus_syl));
            } else {
                strncpy(partial_plus_syl, t->syllable, 510);
            }

            LLMResponse* eow_resp = llm_client_get_syllable_dist(
                ctx->llm, full_recon, partial_plus_syl, 0);
            if (eow_resp) {
                const char** et = (const char**)malloc((size_t)eow_resp->count * sizeof(char*));
                float*       ep = (float*)malloc((size_t)eow_resp->count * sizeof(float));
                if (et && ep) {
                    for (int i = 0; i < eow_resp->count; i++) {
                        et[i] = eow_resp->candidates[i].text;
                        ep[i] = eow_resp->candidates[i].prob;
                    }
                    MeteorDist* eow_dist = meteor_build_dist(et, ep, eow_resp->count, ctx->beta);
                    if (eow_dist) {
                        uint8_t eow_bits[32];
                        int     en = meteor_decode_step(EOW_TOKEN, eow_dist, &prng, ctx->beta, eow_bits);
                        meteor_free_dist(eow_dist);
                        for (int b = 0; b < en && rb_count < MAX_RECOVERED_BITS; b++)
                            recovered_bits[rb_count++] = eow_bits[b];
                    }
                }
                free(et); free(ep);
                llm_response_free(eow_resp);
            }

            if (null_terminator_found(recovered_bits, rb_count)) { done = 1; }

            /* advance full_recon with the completed word */
            char word[512];
            snprintf(word, sizeof(word), "%s%s", partial_word, t->syllable);
            size_t wlen   = strlen(word);
            size_t needed = recon_len + 1 + wlen + 2;
            if (needed > recon_cap) {
                recon_cap = needed * 2;
                full_recon = (char*)realloc(full_recon, recon_cap);
                if (!full_recon) { *out_error = METEOR_ERR_OOM; done = 1; break; }
            }
            if (recon_len > 0) full_recon[recon_len++] = ' ';
            memcpy(full_recon + recon_len, word, wlen);
            recon_len += wlen;
            full_recon[recon_len] = '\0';
            partial_word[0] = '\0';
        } else {
            strncat(partial_word, t->syllable, sizeof(partial_word) - strlen(partial_word) - 1);
        }
    }

    syllabify_text_free(tokens);
    prng_wipe(&prng);
    free(full_recon);

    size_t   msg_len;
    uint8_t* msg = bits_to_bytes(recovered_bits, rb_count, &msg_len);
    free(recovered_bits);

    *out_msg_len = msg_len;
    return msg;
}
