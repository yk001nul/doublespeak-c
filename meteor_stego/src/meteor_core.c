#include "meteor_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── helpers ──────────────────────────────────────────────────────────────── */

static int common_prefix_len(int slot_start, int slot_end, int beta)
{
    if (slot_start == slot_end) return beta;
    int cp = 0;
    for (int i = beta - 1; i >= 0; i--) {
        if (((slot_start >> i) & 1) == ((slot_end >> i) & 1))
            cp++;
        else
            break;
    }
    return cp;
}

/* ── distribution ─────────────────────────────────────────────────────────── */

MeteorDist* meteor_build_dist(const char** syllables, const float* probs,
                               int count, int beta)
{
    if (count <= 0 || beta < 1 || beta > 31) return NULL;

    MeteorDist* dist = (MeteorDist*)calloc(1, sizeof(MeteorDist));
    if (!dist) return NULL;

    dist->slots       = (MeteorSlot*)calloc((size_t)count, sizeof(MeteorSlot));
    dist->count       = count;
    dist->beta        = beta;
    dist->total_slots = 1 << beta;

    if (!dist->slots) { free(dist); return NULL; }

    /* normalise probabilities */
    float sum = 0.0f;
    for (int i = 0; i < count; i++) sum += probs[i];
    if (sum <= 0.0f) sum = 1.0f;

    /*
     * Largest-remainder method: floor each ideal count (no forced minimum
     * yet), then distribute the slots left over by flooring to the
     * candidates with the largest fractional parts. Since ideal[i] sums to
     * exactly total_slots, floor(ideal[i]) can only sum to <= total_slots,
     * so this pass alone can never overflow total_slots.
     */
    int*   counts = (int*)  calloc((size_t)count, sizeof(int));
    float* fracs  = (float*)calloc((size_t)count, sizeof(float));
    if (!counts || !fracs) {
        free(counts); free(fracs);
        free(dist->slots); free(dist);
        return NULL;
    }

    int total = 0;
    for (int i = 0; i < count; i++) {
        float p     = probs[i] / sum;
        float ideal = p * (float)dist->total_slots;
        counts[i]   = (int)floorf(ideal);
        fracs[i]    = ideal - (float)counts[i];
        total      += counts[i];
    }

    while (total < dist->total_slots) {
        int best = 0;
        for (int i = 1; i < count; i++)
            if (fracs[i] > fracs[best]) best = i;
        counts[best]++;
        fracs[best] -= 1.0f;
        total++;
    }
    free(fracs);

    /*
     * Guarantee every candidate keeps at least 1 slot (so it stays
     * reachable in the encoder's r-based slot lookup) by stealing from the
     * largest allocations — this only moves slots around, so it can never
     * push the total above total_slots the way an unconditional "min 1"
     * forced *before* the largest-remainder pass could (that ordering let
     * several near-zero-probability candidates each grab a slot the
     * flooring pass had already spent, overflowing total_slots and leaving
     * the tail candidates with out-of-range/negative-width slots).
     */
    for (int i = 0; i < count; i++) {
        while (counts[i] < 1) {
            int donor = 0;
            for (int j = 1; j < count; j++)
                if (counts[j] > counts[donor]) donor = j;
            if (counts[donor] <= 1) break; /* count > total_slots: no spare slots left */
            counts[donor]--;
            counts[i]++;
        }
    }

    int cursor = 0;
    for (int i = 0; i < count; i++) {
        strncpy(dist->slots[i].text, syllables[i], sizeof(dist->slots[i].text) - 1);
        dist->slots[i].text[sizeof(dist->slots[i].text) - 1] = '\0';
        dist->slots[i].p          = probs[i] / sum;
        dist->slots[i].slot_start = cursor;
        dist->slots[i].slot_end   = cursor + counts[i] - 1;
        dist->slots[i].slot_count = counts[i];
        cursor += counts[i];
    }
    free(counts);

    return dist;
}

void meteor_free_dist(MeteorDist* dist)
{
    if (!dist) return;
    free(dist->slots);
    free(dist);
}

/* ── encode step ──────────────────────────────────────────────────────────── */

MeteorStepResult meteor_encode_step(const MeteorDist* dist,
                                    const uint8_t* msg_bits, size_t bit_offset,
                                    size_t total_bits,
                                    MeteorPRNG* prng, int beta)
{
    MeteorStepResult res;
    memset(&res, 0, sizeof(res));

    uint32_t mask = prng_next_bits(prng, beta);
    res.mask_bits = mask;

    /* read up to beta message bits (pad with 0 if past end) */
    uint32_t msg = 0;
    for (int i = 0; i < beta; i++) {
        msg <<= 1;
        if (bit_offset + (size_t)i < total_bits)
            msg |= msg_bits[bit_offset + i] & 1;
    }

    uint32_t r = msg ^ mask;
    res.r = r;

    /* find the slot containing r */
    int chosen_idx = dist->count - 1;
    for (int i = 0; i < dist->count; i++) {
        if (r >= (uint32_t)dist->slots[i].slot_start &&
            r <= (uint32_t)dist->slots[i].slot_end) {
            chosen_idx = i;
            break;
        }
    }

    strncpy(res.chosen, dist->slots[chosen_idx].text, sizeof(res.chosen) - 1);
    res.chosen[sizeof(res.chosen) - 1] = '\0';

    int start = dist->slots[chosen_idx].slot_start;
    int end   = dist->slots[chosen_idx].slot_end;
    res.cp_len = common_prefix_len(start, end, beta);

    /* store recovered bits (same as the common prefix of msg) */
    for (int i = 0; i < res.cp_len; i++)
        res.recovered[i] = (msg >> (beta - 1 - i)) & 1;

    return res;
}

/* ── decode step ──────────────────────────────────────────────────────────── */

int meteor_decode_step(const char*       chosen_syllable,
                       const MeteorDist* dist,
                       MeteorPRNG*       prng,
                       int               beta,
                       uint8_t*          out_bits)
{
    uint32_t mask = prng_next_bits(prng, beta);

    /* find the slot for this syllable */
    int idx = dist->count - 1;
    for (int i = 0; i < dist->count; i++) {
        if (strcmp(dist->slots[i].text, chosen_syllable) == 0) {
            idx = i;
            break;
        }
    }

    int start  = dist->slots[idx].slot_start;
    int end    = dist->slots[idx].slot_end;
    int cp_len = common_prefix_len(start, end, beta);

    /* common prefix of the slot range XOR with mask prefix */
    uint32_t common = (uint32_t)start >> (beta - cp_len);
    uint32_t msg_prefix = common ^ (mask >> (beta - cp_len));

    for (int i = 0; i < cp_len; i++)
        out_bits[i] = (msg_prefix >> (cp_len - 1 - i)) & 1;

    return cp_len;
}

/* ── content-word history ────────────────────────────────────────────────── */

static const char* WORD_HISTORY_STOPWORDS[] = {
    "a", "an", "the", "to", "in", "on", "at", "of", "and", "or", "but",
    "so", "if", "his", "her", "its", "their", "he", "she", "it", "they",
    "is", "are", "was", "were", "be", "been", "this", "that", "with",
    "for", "as", "by", "not", "no", "do", "does", "did", "has", "have",
    "had", "will", "shall", "can", "could", "would", "should", "may",
    "might", "must", "onto", "from", "into", "up", "down", "out", "off",
    "over", "under", "then", "than", "too", "very", "just", "about",
    "after", "before", "while", "during", "through", "also",
};
#define WORD_HISTORY_STOPWORD_COUNT \
    (sizeof(WORD_HISTORY_STOPWORDS) / sizeof(WORD_HISTORY_STOPWORDS[0]))

static int is_stopword(const char* w, size_t len)
{
    for (size_t i = 0; i < WORD_HISTORY_STOPWORD_COUNT; i++) {
        const char* sw = WORD_HISTORY_STOPWORDS[i];
        if (strlen(sw) == len && strncmp(w, sw, len) == 0) return 1;
    }
    return 0;
}

void meteor_word_history_add(MeteorWordHistory* hist, const char* phrase)
{
    if (!hist || !phrase) return;

    const char* p = phrase;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char* start = p;
        while (*p && *p != ' ') p++;
        size_t len = (size_t)(p - start);

        if (len < 3 || len >= CONTENT_WORD_MAXLEN || is_stopword(start, len))
            continue;

        int dup = 0;
        for (int i = 0; i < hist->count; i++) {
            if (strlen(hist->words[i]) == len &&
                strncmp(hist->words[i], start, len) == 0) { dup = 1; break; }
        }
        if (dup) continue;

        if (hist->count < CONTENT_WORD_HISTORY) {
            memcpy(hist->words[hist->count], start, len);
            hist->words[hist->count][len] = '\0';
            hist->count++;
        } else {
            for (int i = 0; i < CONTENT_WORD_HISTORY - 1; i++)
                memcpy(hist->words[i], hist->words[i + 1], CONTENT_WORD_MAXLEN);
            memcpy(hist->words[CONTENT_WORD_HISTORY - 1], start, len);
            hist->words[CONTENT_WORD_HISTORY - 1][len] = '\0';
        }
    }
}

void meteor_word_history_join(const MeteorWordHistory* hist,
                               char* out, size_t out_size)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!hist) return;

    size_t off = 0;
    for (int i = 0; i < hist->count; i++) {
        int w = snprintf(out + off, out_size - off, "%s%s",
                          i > 0 ? ", " : "", hist->words[i]);
        if (w < 0 || (size_t)w >= out_size - off) break;
        off += (size_t)w;
    }
}

/* ── style question selection ────────────────────────────────────────────── */

static int style_question_is_recent(const StyleQuestionHistory* hist, StyleQuestion q)
{
    if (!hist) return 0;
    for (int i = 0; i < hist->count; i++)
        if (hist->recent[i] == q) return 1;
    return 0;
}

StyleQuestion meteor_draw_style_question(MeteorPRNG* prng,
                                          const StyleQuestionHistory* hist)
{
    StyleQuestion q;
    int attempts = 0;
    do {
        q = (StyleQuestion)(prng_next_bits(prng, 3) % STYLE_Q_COUNT);
        attempts++;
    } while (style_question_is_recent(hist, q) && attempts < STYLE_Q_MAX_REDRAWS);
    return q;
}

void meteor_style_question_history_push(StyleQuestionHistory* hist, StyleQuestion q)
{
    if (!hist) return;
    if (hist->count < STYLE_Q_HISTORY) {
        hist->recent[hist->count++] = q;
    } else {
        memmove(hist->recent, hist->recent + 1,
                (size_t)(STYLE_Q_HISTORY - 1) * sizeof(StyleQuestion));
        hist->recent[STYLE_Q_HISTORY - 1] = q;
    }
}

/* ── clause-ending selection ─────────────────────────────────────────────── */

int meteor_draw_clause_end(MeteorPRNG* prng, const ClauseState* state)
{
    uint32_t v = prng_next_bits(prng, CLAUSE_END_DRAW_BITS); /* always drawn */

    if (state->phrases_in_sentence + 1 < CLAUSE_END_MIN_PHRASES) return 0;
    if (state->phrases_in_sentence + 1 >= CLAUSE_END_MAX_PHRASES) return 1;
    return v == 0;
}

/* ── digression axis selection ───────────────────────────────────────────── */

static int digression_axis_is_recent(const DigressionAxisHistory* hist, DigressionAxis a)
{
    if (!hist) return 0;
    for (int i = 0; i < hist->count; i++)
        if (hist->recent[i] == a) return 1;
    return 0;
}

DigressionAxis meteor_draw_digression_axis(MeteorPRNG* prng,
                                            const DigressionAxisHistory* hist)
{
    DigressionAxis a;
    int attempts = 0;
    do {
        a = (DigressionAxis)(prng_next_bits(prng, 3) % DIGRESS_AXIS_COUNT);
        attempts++;
    } while (digression_axis_is_recent(hist, a) && attempts < DIGRESS_AXIS_MAX_REDRAWS);
    return a;
}

void meteor_digression_axis_history_push(DigressionAxisHistory* hist, DigressionAxis a)
{
    if (!hist) return;
    if (hist->count < DIGRESS_AXIS_HISTORY) {
        hist->recent[hist->count++] = a;
    } else {
        memmove(hist->recent, hist->recent + 1,
                (size_t)(DIGRESS_AXIS_HISTORY - 1) * sizeof(DigressionAxis));
        hist->recent[DIGRESS_AXIS_HISTORY - 1] = a;
    }
}

/* ── digression mode selection ───────────────────────────────────────────── */

int meteor_draw_digress_mode(MeteorPRNG* prng, const DigressionState* state)
{
    uint32_t v = prng_next_bits(prng, DIGRESS_DRAW_BITS); /* always drawn */

    if (state->topic_sentences_completed < DIGRESS_MIN_TOPIC_SENTENCES) return 0;
    if (state->last_was_digression) return 0; /* cap digression at one hop */
    return v == 0;
}

int meteor_draw_digression_variant(MeteorPRNG* prng)
{
    return (int)(prng_next_bits(prng, DIGRESS_VARIANT_DRAW_BITS) % DIGRESS_VARIANT_COUNT);
}
