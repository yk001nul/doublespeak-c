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

/*
 * Per-step decision of whether to end the current sentence (append "."
 * and start a fresh one) instead of continuing it. Independent PRNG draw
 * from StyleQuestion, taken at the same relative point in the loop on
 * both encode.c and decode.c. See meteor_draw_clause_end() for the
 * min/max phrase-per-sentence bounds.
 */
#define CLAUSE_END_MIN_PHRASES 2  /* never end right after the opener alone */
#define CLAUSE_END_MAX_PHRASES 3  /* force an end so run-ons stay bounded; lowered
                                     from 6 — stacking up to 5 PRNG-selected
                                     prepositional modifiers per sentence produced
                                     word-salad ("to avoid X to Y by Z with W").
                                     Shorter sentences also re-anchor the subject
                                     to the topic more often. */
#define CLAUSE_END_DRAW_BITS   3  /* v==0 out of 8 possible values ⇒ ~1/8 chance/step */

typedef struct {
    int phrases_in_sentence;
} ClauseState;

/*
 * Decide whether the phrase about to be generated should be the last one
 * in the current sentence. Always draws from prng (even when the min/max
 * bound forces the outcome), so PRNG stream position stays identical
 * between encode and decode regardless of sentence length. Call once per
 * step, after meteor_draw_style_question(), before the beta-bit slot
 * draw inside meteor_encode_step()/meteor_decode_step().
 */
int meteor_draw_clause_end(MeteorPRNG* prng, const ClauseState* state);

/*
 * Axis of a sentence-level digression: instead of paraphrasing the fixed
 * topic, the opening phrase of the sentence is generated from a two-stage
 * process — (1) a non-bit-embedding question is asked internally (never
 * written to the covertext) about a secondary subject/object mentioned in
 * the text so far, per this axis, yielding a one-off answer text; (2) that
 * answer is fed through the normal paraphrase-candidate/beta-bit-selection
 * machinery exactly like the main topic, so the covertext ends up
 * containing a paraphrase of the answer, not the question. Only applies
 * at sentence-opening steps (subject_anchor empty), never at continuation
 * steps. See llm_client_get_digression_answer().
 */
typedef enum {
    DIGRESS_AXIS_DESCRIBE = 0,  /* appearance/property: "what color/shape/kind" */
    DIGRESS_AXIS_STATE,         /* condition/status: "is it still running" */
    DIGRESS_AXIS_SIGNIFICANCE,  /* importance/seriousness/consequence */
    DIGRESS_AXIS_ORIGIN,        /* cause/history/reason it exists */
    DIGRESS_AXIS_OUTCOME,       /* what happens to it next / implication */
    DIGRESS_AXIS_COUNT
} DigressionAxis;

/*
 * Rolling history of recently-drawn digression axes, used only to bias
 * the redraw in meteor_draw_digression_axis() away from immediate
 * repeats. Same determinism rationale as StyleQuestionHistory.
 */
#define DIGRESS_AXIS_HISTORY     1  /* forbid repeating the immediately-previous axis */
#define DIGRESS_AXIS_MAX_REDRAWS 4  /* deterministic cap; a redraw loop always terminates */

typedef struct {
    DigressionAxis recent[DIGRESS_AXIS_HISTORY];
    int            count;
} DigressionAxisHistory;

/*
 * Draw the next digression axis from prng (3 bits, mod DIGRESS_AXIS_COUNT),
 * redrawing up to DIGRESS_AXIS_MAX_REDRAWS times if it collides with hist.
 * Must be called exactly once per loop iteration, at the same point
 * relative to the other per-step draws, on both encode.c and decode.c —
 * same lockstep requirement as meteor_draw_style_question().
 */
DigressionAxis meteor_draw_digression_axis(MeteorPRNG* prng,
                                            const DigressionAxisHistory* hist);

/* Pushes a into hist's rolling window (evicting the oldest if full). */
void meteor_digression_axis_history_push(DigressionAxisHistory* hist, DigressionAxis a);

/*
 * Per-sentence digression state: tracks how many topic-anchored (non-
 * digression) sentences have completed, and whether the most recently
 * completed sentence was itself a digression, so digression can be
 * capped at one hop (a digression sentence is always followed by a
 * forced topic-anchored sentence) and gated behind a minimum number of
 * topic sentences at the start of the covertext.
 */
#define DIGRESS_MIN_TOPIC_SENTENCES 1  /* >=1 topic sentence before first digression is eligible */
#define DIGRESS_DRAW_BITS           3  /* v==0 out of 8 possible values ⇒ ~1/8 chance/step */

typedef struct {
    int topic_sentences_completed; /* count of completed topic-anchored sentences (MIN gate) */
    int last_was_digression;       /* 1 if the most recently completed sentence was a digression */
} DigressionState;

/*
 * Decide whether the sentence about to be opened should digress onto a
 * secondary entity instead of paraphrasing the topic. Always draws from
 * prng (even when the min-topic-sentences gate or the cap-at-one-hop
 * rule forces the outcome), so PRNG stream position stays identical
 * between encode and decode regardless of digression history. Call once
 * per step (every phrase, not just sentence-opening steps), at the same
 * relative point as meteor_draw_clause_end() — its result is only
 * consulted on steps where subject_anchor is empty, and discarded
 * otherwise, exactly like StyleQuestion on opening steps.
 */
int meteor_draw_digress_mode(MeteorPRNG* prng, const DigressionState* state);

/*
 * Picks which of DIGRESS_VARIANT_COUNT alternate phrasings of the chosen
 * DigressionAxis's internal question to ask, purely for variety (so the
 * same axis doesn't always ask the identical question wording across a
 * long message). Always drawn from prng every step (lockstep, same as
 * meteor_draw_digress_mode()); only consulted on sentence-opening steps
 * where digression fires.
 */
#define DIGRESS_VARIANT_COUNT     2
#define DIGRESS_VARIANT_DRAW_BITS 1

int meteor_draw_digression_variant(MeteorPRNG* prng);
