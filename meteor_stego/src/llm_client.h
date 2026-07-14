#pragma once

#include "meteor_core.h"
#include <stddef.h>

typedef struct {
    char  text[64];
    float prob;
} LLMCandidate;

typedef struct {
    LLMCandidate* candidates;
    int           count;
} LLMResponse;

typedef struct {
    char  base_url[256];
    int   timeout_ms;
    int   max_candidates;
    void* curl_handle;     /* CURL* — opaque to callers */
} LLMClient;

LLMClient*   llm_client_create(const char* base_url, int max_candidates, int timeout_ms);
void         llm_client_destroy(LLMClient* client);

/*
 * Build an embellishment preamble string from a style id and topic text.
 * style   : MeteorStyle cast to int; pass 0 (METEOR_STYLE_NONE) to get NULL.
 * topic   : the starting_context string repurposed as topic source.
 * Returns a heap-allocated string the caller must free(), or NULL if style==0
 * or on OOM.  The returned string is prepended to every LLM prompt.
 */
char* llm_client_build_preamble(int style, const char* topic);

/*
 * Return a fixed sentence-seed string for the given style (e.g. "I" for chat,
 * "Scientists" for news).  The encoder prepends this to full_text before the
 * first Meteor step so distributions are conditioned on a natural sentence
 * start.  The decoder skips the seed words in the covertext.
 * Returns NULL for METEOR_STYLE_NONE.  The returned pointer is a string literal
 * — do NOT free() it.
 */
const char* llm_client_style_seed(int style);

/*
 * Get syllable distribution for one Meteor step.
 * preamble       : style/topic prefix from llm_client_build_preamble(), or NULL
 * full_context   : generated text so far (not including preamble)
 * partial_word   : syllables built for the current word so far (empty if is_new_word)
 * is_new_word    : 1 = first syllable of a new word, 0 = continuation / EOW step
 *
 * Returns NULL on unrecoverable failure (caller treats it as METEOR_ERR_LLM).
 * On soft failure (bad JSON), returns a uniform distribution over num_candidates slots.
 * Caller frees with llm_response_free().
 */
LLMResponse* llm_client_get_syllable_dist(LLMClient*  client,
                                           const char* preamble,
                                           const char* full_context,
                                           const char* partial_word,
                                           int         is_new_word);

/*
 * Get a distribution over whole words for the word-level Meteor step.
 * Used in embellishment (style) mode instead of syllable-level sampling.
 * preamble: style/topic prefix from llm_client_build_preamble(), or NULL.
 * full_context: generated text so far.
 * Returns NULL on unrecoverable failure; uniform fallback on soft failure.
 */
/*
 * blacklist_word: word chosen in the previous step; the LLM is asked not to
 * suggest it again, breaking single-word fixation at temp=0.  Pass NULL on
 * the first step.
 */
LLMResponse* llm_client_get_word_dist(LLMClient*  client,
                                       const char* preamble,
                                       const char* full_context,
                                       const char* blacklist_word);

/*
 * Get a distribution over multi-word phrases for one Meteor step (style mode).
 * Returns N phrase candidates (e.g. "drives to work", "takes the bus") with
 * probabilities.  The grammar forces lowercase-only multi-word keys.
 * preamble: style/topic preamble from llm_client_build_preamble(), or NULL.
 * full_context: covertext generated so far.
 * blacklist_phrases: comma-separated phrases chosen in recent prior steps;
 *   suppress repeating any of them. NULL/empty on the first step.
 * blacklist_words: comma-separated individual content words (nouns/verbs/
 *   adjectives) drawn from recently chosen phrases; suppress reusing any of
 *   them even inside a new, otherwise-unseen phrase. NULL/empty on the
 *   first step. See meteor_word_history_add()/_join() in meteor_core.h.
 * subject_anchor: the first chosen phrase (which is forced to open with an
 *   explicit subject), named verbatim in the prompt so every later step has
 *   a concrete subject to stay consistent with. NULL on the first step.
 * question: which question (STYLE_Q_HOW/WHERE/WHO_MEET/WHO_AVOID/WHY) the
 *   continuation should answer, drawn via meteor_draw_style_question().
 *   Ignored when full_context is empty (opening step uses its own
 *   subject-establishing instruction instead).
 * style: the MeteorStyle value (cast to int) — injects a per-style register
 *   hint into every phrase prompt so the four styles differentiate in word
 *   choice (the preamble's single style descriptor is too weak on its own).
 *   Values outside 1-4 inject nothing. Part of the shared encode/decode
 *   prompt: both sides must pass the same value (they both read it from
 *   MeteorConfig.style, which is already shared protocol state).
 * A digression sentence's opening step is not special-cased by this
 * function at all — the caller passes a temporary preamble (built via
 * llm_client_build_preamble() on the stage-1 answer from
 * llm_client_get_digression_answer() below) in place of the main preamble
 * for that one call, so it takes the exact same code path as a normal
 * topic-anchored opening.
 * Returns NULL on unrecoverable failure; uniform fallback on soft failure.
 */
LLMResponse* llm_client_get_phrase_dist(LLMClient*  client,
                                          const char* preamble,
                                          const char* full_context,
                                          const char* blacklist_phrases,
                                          const char* blacklist_words,
                                          const char* subject_anchor,
                                          StyleQuestion question,
                                          int style);

/*
 * Stage 1 of a sentence-level digression (style mode): asks a plain,
 * non-bit-embedding question about a secondary entity from the text so
 * far (per axis, one of DIGRESS_VARIANT_COUNT phrasings picked by
 * variant) and returns the model's one-sentence answer. This call never
 * carries message-payload bits — no grammar/candidate distribution, just
 * one deterministic /completion round-trip (temp 0, seed 42), so both
 * encode.c and decode.c get byte-identical text given identical
 * full_context/axis/variant.
 * full_context: covertext generated so far (same value passed as
 *   full_context elsewhere).
 * axis: which DigressionAxis question to ask, drawn via
 *   meteor_draw_digression_axis().
 * variant: which of DIGRESS_VARIANT_COUNT phrasings of that axis's
 *   question to use, drawn via meteor_draw_digression_variant().
 * The caller feeds the returned text into llm_client_build_preamble() and
 * then the normal llm_client_get_phrase_dist() opening-step path (stage
 * 2) to actually embed bits while paraphrasing the answer.
 * Returns NULL only on OOM; HTTP/parse failures fall back internally to a
 * fixed deterministic string so encode/decode stay in lockstep. Caller
 * frees the returned string.
 */
char* llm_client_get_digression_answer(LLMClient* client, const char* full_context,
                                       DigressionAxis axis, int variant);

void llm_response_free(LLMResponse* resp);

/* Returns 1 if the server at base_url/health responds OK, 0 otherwise. */
int llm_client_health(LLMClient* client);
