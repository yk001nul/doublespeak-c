#pragma once

#include <stddef.h>
#include "../include/meteor.h"

typedef struct {
    char  text[64];
    float prob;
} LLMCandidate;

typedef struct {
    LLMCandidate* candidates;
    int           count;
} LLMResponse;

typedef struct {
    char               base_url[256];
    int                timeout_ms;
    int                max_candidates;
    MeteorChatTemplate chat_template;
    void*              curl_handle;     /* CURL* — opaque to callers */
} LLMClient;

LLMClient*   llm_client_create(const char* base_url, int max_candidates, int timeout_ms,
                                MeteorChatTemplate chat_template);
void         llm_client_destroy(LLMClient* client);

/*
 * Get syllable distribution for one Meteor step.
 * full_context   : entire generated text so far (used as prompt)
 * partial_word   : syllables built for the current word so far (empty string if is_new_word)
 * is_new_word    : 1 = first syllable of a new word, 0 = continuation / EOW step
 *
 * Returns NULL on unrecoverable failure (caller treats it as METEOR_ERR_LLM).
 * On soft failure (bad JSON), returns a uniform distribution over num_candidates slots.
 * Caller frees with llm_response_free().
 */
LLMResponse* llm_client_get_syllable_dist(LLMClient*  client,
                                           const char* full_context,
                                           const char* partial_word,
                                           int         is_new_word);

void llm_response_free(LLMResponse* resp);

/* Returns 1 if the server at base_url/health responds OK, 0 otherwise. */
int llm_client_health(LLMClient* client);

/*
 * Erase the KV cache for the given slot (POST /slots/{id_slot} {"action":"erase"}).
 * Returns 1 on success, 0 if the endpoint is unavailable or the request fails.
 * Call between encode and decode phases to clear accumulated server state.
 */
int llm_client_erase_slot(LLMClient* client, int id_slot);

/*
 * Trace support — call once at the start of each encode/decode phase.
 * If the METEOR_TRACE env-var is set, prints a "=== phase ===" header and
 * resets the per-session call counter so encoder and decoder calls align.
 */
void llm_client_trace_phase(const char* phase);
