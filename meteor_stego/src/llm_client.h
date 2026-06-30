#pragma once

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
LLMResponse* llm_client_get_word_dist(LLMClient*  client,
                                       const char* preamble,
                                       const char* full_context);

void llm_response_free(LLMResponse* resp);

/* Returns 1 if the server at base_url/health responds OK, 0 otherwise. */
int llm_client_health(LLMClient* client);
