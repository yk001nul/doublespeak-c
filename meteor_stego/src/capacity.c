#include "../include/meteor.h"
#include "meteor_ctx.h"
#include "meteor_core.h"
#include "llm_client.h"

#include <stdlib.h>
#include <string.h>

/* ── helpers ──────────────────────────────────────────────────────────────── */

static int count_words(const char* s)
{
    if (!s || !*s) return 0;
    int count = 0, in_word = 0;
    for (; *s; s++) {
        if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
            in_word = 0;
        else if (!in_word) {
            in_word = 1;
            count++;
        }
    }
    return count;
}

/*
 * How many words does a paraphrase in this style produce relative to the
 * source sentence?  Informal chat adds filler; news tends to be tighter.
 */
static float style_expansion(MeteorStyle style)
{
    switch (style) {
        case METEOR_STYLE_INFORMAL_CHAT: return 1.4f;
        case METEOR_STYLE_CASUAL_BLOG:   return 1.3f;
        case METEOR_STYLE_FORMAL_EMAIL:  return 1.2f;
        case METEOR_STYLE_NEWS_ARTICLE:  return 1.1f;
        default:                         return 1.0f;
    }
}

/*
 * Common-prefix length between two slot indices in a beta-wide range.
 * Identical to the static helper in meteor_core.c — duplicated here to avoid
 * exposing it in the internal header.
 */
static int slot_cp_len(int a, int b, int beta)
{
    if (a == b) return beta;
    int cp = 0;
    for (int i = beta - 1; i >= 0; i--) {
        if (((a >> i) & 1) == ((b >> i) & 1))
            cp++;
        else
            break;
    }
    return cp;
}

/*
 * E[cp_len] for one Meteor step given a fully built distribution.
 * Weights each candidate by its PRNG hit probability (slot_count / 2^beta).
 */
static float expected_cp_len(const MeteorDist* dist, int beta)
{
    float total = (float)dist->total_slots;
    float e     = 0.0f;
    for (int i = 0; i < dist->count; i++) {
        float hit_prob = (float)dist->slots[i].slot_count / total;
        int   cp       = slot_cp_len(dist->slots[i].slot_start,
                                     dist->slots[i].slot_end, beta);
        e += hit_prob * (float)cp;
    }
    return e;
}

/* ── public API ───────────────────────────────────────────────────────────── */

int meteor_estimate_capacity(MeteorCtx*              ctx,
                             const char*             context,
                             int                     sample_steps,
                             MeteorCapacityEstimate* out)
{
    if (!ctx || !context || !out) return METEOR_ERR_CONFIG;
    memset(out, 0, sizeof(*out));

    int ctx_words = count_words(context);
    if (ctx_words == 0) return METEOR_ERR_CONFIG;

    int   est_words    = (int)(ctx_words * style_expansion(ctx->style) + 0.5f);
    float avg_bits     = 0.0f;
    int   used_samples = 0;

    if (sample_steps > 0 && ctx->style != METEOR_STYLE_NONE && ctx->llm) {
        char* preamble   = llm_client_build_preamble((int)ctx->style, context);
        const char* seed = llm_client_style_seed((int)ctx->style);

        char full_text[4096] = {0};
        if (seed) strncpy(full_text, seed, sizeof(full_text) - 1);

        char  last_word[64] = {0};
        float total_cp      = 0.0f;

        for (int s = 0; s < sample_steps; s++) {
            LLMResponse* resp = llm_client_get_word_dist(
                ctx->llm, preamble, full_text,
                last_word[0] ? last_word : NULL);
            if (!resp) { free(preamble); return METEOR_ERR_LLM; }

            const char** texts = (const char**)malloc((size_t)resp->count * sizeof(char*));
            float*       probs = (float*)       malloc((size_t)resp->count * sizeof(float));
            if (texts && probs) {
                for (int i = 0; i < resp->count; i++) {
                    texts[i] = resp->candidates[i].text;
                    probs[i] = resp->candidates[i].prob;
                }
                MeteorDist* dist = meteor_build_dist(texts, probs, resp->count, ctx->beta);
                if (dist) {
                    total_cp += expected_cp_len(dist, ctx->beta);
                    meteor_free_dist(dist);
                    used_samples++;
                }
            }
            free(texts);
            free(probs);

            /* advance full_text with the highest-prob word so the next sample
               sees realistic context, just like the encoder does */
            strncpy(last_word, resp->candidates[0].text, sizeof(last_word) - 1);
            size_t ft = strlen(full_text);
            size_t wl = strlen(last_word);
            if (ft + 1 + wl + 1 < sizeof(full_text)) {
                full_text[ft] = ' ';
                memcpy(full_text + ft + 1, last_word, wl + 1);
            }

            llm_response_free(resp);
        }

        free(preamble);
        avg_bits = used_samples > 0 ? total_cp / (float)used_samples : 0.0f;
    } else {
        /* Heuristic: assume a conservative 65 % of the theoretical maximum.
           At temp=0.0 the model is greedy and concentrates mass, so pure
           theoretical (beta bits/step) is rarely achieved in practice. */
        avg_bits = (float)ctx->beta * 0.65f;
    }

    out->estimated_words   = est_words;
    out->avg_bits_per_word = avg_bits;
    out->estimated_bits    = (int)((float)est_words * avg_bits);
    out->estimated_bytes   = out->estimated_bits / 8;
    out->sample_steps_used = used_samples;

    return METEOR_OK;
}
