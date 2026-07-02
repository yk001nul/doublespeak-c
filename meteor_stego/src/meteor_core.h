#pragma once

#include "prng.h"
#include "bits.h"
#include <stdint.h>

/* Internal end-of-word marker — never written to output covertext. */
#define EOW_TOKEN "\x01"

/* One entry in a syllable distribution. */
typedef struct {
    char    text[64];
    float   p;
    int     slot_start;
    int     slot_end;
    int     slot_count;
} MeteorSlot;

typedef struct {
    MeteorSlot* slots;
    int         count;
    int         beta;
    int         total_slots; /* 2^beta */
} MeteorDist;

/* Result of one encode/decode step. */
typedef struct {
    char     chosen[64];
    uint32_t mask_bits;
    uint32_t r;
    int      cp_len;           /* common prefix length (bits recovered) */
    uint8_t  recovered[32];   /* recovered bits, length = cp_len */
} MeteorStepResult;

/*
 * Build slot table from a probability distribution.
 * syllables[i] / probs[i]: syllable string and its probability (must be > 0).
 * beta: bits per step.
 * Normalises probabilities and assigns integer slot ranges in [0, 2^beta).
 * Caller frees with meteor_free_dist().
 */
MeteorDist* meteor_build_dist(const char** syllables, const float* probs,
                               int count, int beta);
void        meteor_free_dist(MeteorDist* dist);

/*
 * Encode one step: consumes up to beta bits from msg_bits[bit_offset..].
 * Advances prng state by beta bits.
 */
MeteorStepResult meteor_encode_step(const MeteorDist* dist,
                                    const uint8_t* msg_bits, size_t bit_offset,
                                    size_t total_bits,
                                    MeteorPRNG* prng, int beta);

/*
 * Decode one step: given the chosen syllable and distribution, recover bits.
 * Advances prng state by beta bits.
 * Writes recovered bits (0/1 per byte) to out_bits[0..cp_len).
 * Returns number of bits recovered.
 */
int meteor_decode_step(const char*       chosen_syllable,
                       const MeteorDist* dist,
                       MeteorPRNG*       prng,
                       int               beta,
                       uint8_t*          out_bits);

/*
 * Rolling content-word history for phrase-level repetition avoidance.
 * Tracks up to CONTENT_WORD_HISTORY distinct non-stopword tokens seen
 * across recently chosen phrases (oldest evicted first). Shared between
 * encode.c and decode.c so both sides build a byte-identical blacklist —
 * any divergence here would corrupt the LLM prompt and desync decoding.
 */
#define CONTENT_WORD_HISTORY 8
#define CONTENT_WORD_MAXLEN  24

typedef struct {
    char words[CONTENT_WORD_HISTORY][CONTENT_WORD_MAXLEN];
    int  count;
} MeteorWordHistory;

/* Tokenizes phrase on spaces and adds each non-stopword, non-duplicate
 * token (length in [3, CONTENT_WORD_MAXLEN)) to the rolling history. */
void meteor_word_history_add(MeteorWordHistory* hist, const char* phrase);

/* Writes a comma-joined blacklist string into out (size out_size).
 * Writes "" if hist is empty. */
void meteor_word_history_join(const MeteorWordHistory* hist,
                               char* out, size_t out_size);

/*
 * Per-step "question" that steers what a continuation phrase answers
 * (method / destination / person met / person avoided / motivation),
 * used in place of an open-ended "continue naturally" instruction to
 * give the LLM's candidates a concrete, narrow axis of variation.
 * Not used for the opening step of a sentence (no continuation yet).
 */
typedef enum {
    STYLE_Q_HOW = 0,      /* method / means / manner */
    STYLE_Q_WHERE,         /* destination / origin / location */
    STYLE_Q_WHO_MEET,      /* person/group to meet or involve */
    STYLE_Q_WHO_AVOID,     /* person/thing to avoid or evade */
    STYLE_Q_WHY,           /* motivation / purpose / reason */
    STYLE_Q_COUNT
} StyleQuestion;

/*
 * Rolling history of recently-drawn questions, used only to bias the
 * redraw in meteor_draw_style_question() away from immediate repeats.
 * Shared between encode.c and decode.c for the same determinism reason
 * as MeteorWordHistory: both sides must draw/redraw identically or the
 * PRNG streams desync.
 */
#define STYLE_Q_HISTORY     1  /* forbid repeating the immediately-previous question */
#define STYLE_Q_MAX_REDRAWS 4  /* deterministic cap; a redraw loop always terminates */

typedef struct {
    StyleQuestion recent[STYLE_Q_HISTORY];
    int           count;
} StyleQuestionHistory;

/*
 * Draw the next question from prng (3 bits, mod STYLE_Q_COUNT), redrawing
 * up to STYLE_Q_MAX_REDRAWS times if it collides with hist. Must be called
 * exactly once per loop iteration, at the same point relative to the
 * existing beta-bit slot-selection draw, on both the encode and decode
 * side — see the PRNG lockstep note in ARCHITECTURE.md §6.
 */
StyleQuestion meteor_draw_style_question(MeteorPRNG* prng,
                                          const StyleQuestionHistory* hist);

/* Pushes q into hist's rolling window (evicting the oldest if full). */
void meteor_style_question_history_push(StyleQuestionHistory* hist, StyleQuestion q);
