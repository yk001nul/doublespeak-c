#ifndef METEOR_H
#define METEOR_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaque context ──────────────────────────────────────────────────────── */

typedef struct MeteorCtx MeteorCtx;

/* ── Embellishment style ─────────────────────────────────────────────────── */

/*
 * Controls how the LLM frames the generated covertext.
 * METEOR_STYLE_NONE (0) = legacy mode: starting_context appears verbatim at
 *   the start of the covertext and the LLM continues it neutrally.
 * Any other value = embellishment mode: starting_context is used as a topic
 *   source (injected into the LLM prompt but NOT written to the covertext).
 *   The covertext is generated fresh in the chosen style.
 */
typedef enum {
    METEOR_STYLE_NONE = 0,
    METEOR_STYLE_INFORMAL_CHAT,   /* casual instant / mobile messaging */
    METEOR_STYLE_FORMAL_EMAIL,    /* professional business email prose */
    METEOR_STYLE_CASUAL_BLOG,     /* relaxed first-person blog writing */
    METEOR_STYLE_NEWS_ARTICLE,    /* neutral third-person news prose */
} MeteorStyle;

/* ── Configuration ───────────────────────────────────────────────────────── */

typedef struct {
    /*
     * Key material — provide ONE of the following two options:
     *
     * Option A: raw 256-bit key (already derived by caller, e.g. from ECDH)
     *   Set key_raw to point to 32 bytes; set key_input / key_input_len to NULL/0.
     *
     * Option B: arbitrary input material (passphrase, RSA shared secret, …)
     *   Set key_input / key_input_len; set key_raw to NULL.
     *   The library derives a 256-bit key via HKDF-SHA256 internally.
     */
    const uint8_t* key_raw;          /* 32 bytes, or NULL */
    const uint8_t* key_input;        /* arbitrary bytes, or NULL */
    size_t         key_input_len;
    const uint8_t* salt;             /* 32-byte HKDF salt (shared out-of-band); NULL = zero salt */
    size_t         salt_len;

    int         beta;            /* bits per Meteor step (2–5 recommended; default 3) */
    int         num_candidates;  /* syllable candidates per LLM call (4–8; default 6) */
    const char* llm_url;         /* local server URL, e.g. "http://127.0.0.1:8080" */
    const char* hyphen_dict;     /* path to hyph_en_US.dic (NULL = use heuristic) */
    int         max_steps;       /* max syllable steps before giving up (default 256) */
    int         llm_timeout_ms;  /* HTTP timeout per LLM call (default 30000) */

    MeteorStyle style;           /* embellishment style (default METEOR_STYLE_NONE) */
} MeteorConfig;

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

MeteorCtx* meteor_create(const MeteorConfig* config);
void       meteor_destroy(MeteorCtx* ctx);

/* ── Encode ──────────────────────────────────────────────────────────────── */

/*
 * Encodes message into covertext.
 * starting_context:
 *   METEOR_STYLE_NONE — plain text prepended verbatim to the covertext and
 *     used as the LLM seed (e.g. "Researchers announced").
 *   Any other style   — topic/idea source injected into the LLM prompt but
 *     NOT written to the covertext; the output is generated fresh in the
 *     configured style (e.g. "i like burger. burger good.").
 *   Both sides must supply the identical starting_context.
 * Returns heap-allocated null-terminated covertext string on success, NULL on error.
 * Caller must call meteor_free() on the returned pointer.
 */
char* meteor_encode(MeteorCtx*     ctx,
                    const uint8_t* message,
                    size_t         msg_len,
                    const char*    starting_context,
                    int*           out_error);

/* ── Decode ──────────────────────────────────────────────────────────────── */

/*
 * Decodes covertext back to message bytes.
 * starting_context: must be identical to the one used during encode.
 * Returns heap-allocated message bytes on success, NULL on error.
 * Caller must call meteor_free() on the returned pointer.
 * out_msg_len: set to the number of recovered bytes.
 */
uint8_t* meteor_decode(MeteorCtx*  ctx,
                       const char* covertext,
                       const char* starting_context,
                       size_t*     out_msg_len,
                       int*        out_error);

/* ── Utilities ───────────────────────────────────────────────────────────── */

/*
 * Syllabify a single word using libhyphen (or heuristic fallback).
 * Returns null-terminated string with syllables joined by middle-dot U+00B7
 * e.g. "re·mark·a·ble". Caller must call meteor_free().
 */
char* meteor_syllabify_word(MeteorCtx* ctx, const char* word);

/* Check LLM server health. Returns 1 if reachable, 0 otherwise. */
int meteor_llm_health(MeteorCtx* ctx);

/* Free any pointer returned by this library. */
void meteor_free(void* ptr);

/* ── Error codes ─────────────────────────────────────────────────────────── */

#define METEOR_OK              0
#define METEOR_ERR_CONFIG      1   /* invalid configuration */
#define METEOR_ERR_LLM         2   /* LLM server unreachable or returned bad response */
#define METEOR_ERR_CAPACITY    3   /* message too long for max_steps */
#define METEOR_ERR_DECODE      4   /* syllabification or distribution mismatch */
#define METEOR_ERR_DICT        5   /* hyphenation dictionary not found */
#define METEOR_ERR_OOM         6   /* memory allocation failed */
#define METEOR_ERR_TIMEOUT     7   /* LLM call timed out */
#define METEOR_ERR_CRYPTO      8   /* libsodium initialisation or HKDF failure */

#ifdef __cplusplus
}
#endif

#endif /* METEOR_H */
