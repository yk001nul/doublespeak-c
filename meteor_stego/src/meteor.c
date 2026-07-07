#include "../include/meteor.h"
#include "meteor_ctx.h"
#include "prng.h"
#include "llm_client.h"
#include "syllabifier.h"
#include "encode.h"
#include "decode.h"

#include <stdlib.h>
#include <string.h>
#include <sodium.h>

/* ── lifecycle ────────────────────────────────────────────────────────────── */

MeteorCtx* meteor_create(const MeteorConfig* config)
{
    if (!config) return NULL;
    if (sodium_init() < 0) return NULL;

    /* validate key material */
    if (!config->key_raw && (!config->key_input || config->key_input_len == 0))
        return NULL;

    MeteorCtx* ctx = (MeteorCtx*)calloc(1, sizeof(MeteorCtx));
    if (!ctx) return NULL;

    /* derive or copy key */
    if (config->key_raw) {
        memcpy(ctx->key, config->key_raw, 32);
    } else {
        MeteorPRNG tmp;
        if (prng_init(&tmp,
                      config->key_input, config->key_input_len,
                      config->salt, config->salt_len) != 0) {
            free(ctx);
            return NULL;
        }
        memcpy(ctx->key, tmp.key, 32);
        prng_wipe(&tmp);
    }

    if (config->salt && config->salt_len > 0) {
        size_t slen = config->salt_len < 32 ? config->salt_len : 32;
        memcpy(ctx->salt, config->salt, slen);
        ctx->salt_len = slen;
    }

    ctx->beta           = config->beta          > 0 ? config->beta          : 3;
    ctx->num_candidates = config->num_candidates > 0 ? config->num_candidates : 6;
    ctx->max_steps      = config->max_steps      > 0 ? config->max_steps      : 256;
    ctx->llm_timeout_ms = config->llm_timeout_ms > 0 ? config->llm_timeout_ms : 30000;

    strncpy(ctx->llm_url,
            config->llm_url ? config->llm_url : "http://127.0.0.1:8080",
            sizeof(ctx->llm_url) - 1);

    if (config->hyphen_dict)
        strncpy(ctx->hyphen_dict_path, config->hyphen_dict,
                sizeof(ctx->hyphen_dict_path) - 1);

    ctx->style = config->style;

    ctx->llm = llm_client_create(ctx->llm_url, ctx->num_candidates, ctx->llm_timeout_ms);
    if (!ctx->llm) { free(ctx); return NULL; }

    ctx->syl = syllabifier_create(ctx->hyphen_dict_path[0] ? ctx->hyphen_dict_path : NULL);
    if (!ctx->syl) {
        llm_client_destroy(ctx->llm);
        free(ctx);
        return NULL;
    }

    return ctx;
}

void meteor_destroy(MeteorCtx* ctx)
{
    if (!ctx) return;
    llm_client_destroy(ctx->llm);
    syllabifier_destroy(ctx->syl);
    sodium_memzero(ctx, sizeof(MeteorCtx));
    free(ctx);
}

/* ── encode / decode ──────────────────────────────────────────────────────── */

char* meteor_encode(MeteorCtx*     ctx,
                    const uint8_t* message,
                    size_t         msg_len,
                    const char*    starting_context,
                    int*           out_error)
{
    return meteor_encode_ex(ctx, message, msg_len, starting_context,
                            NULL, NULL, out_error);
}

char* meteor_encode_ex(MeteorCtx*       ctx,
                       const uint8_t*    message,
                       size_t            msg_len,
                       const char*       starting_context,
                       MeteorProgressFn  progress_cb,
                       void*             progress_userdata,
                       int*              out_error)
{
    int dummy;
    if (!out_error) out_error = &dummy;
    if (!ctx || !message) { *out_error = METEOR_ERR_CONFIG; return NULL; }
    return meteor_encode_impl(ctx, message, msg_len, starting_context,
                              progress_cb, progress_userdata, out_error);
}

uint8_t* meteor_decode(MeteorCtx*  ctx,
                       const char* covertext,
                       const char* starting_context,
                       size_t*     out_msg_len,
                       int*        out_error)
{
    int    dummy_err;
    size_t dummy_len;
    if (!out_error)   out_error   = &dummy_err;
    if (!out_msg_len) out_msg_len = &dummy_len;
    if (!ctx || !covertext) { *out_error = METEOR_ERR_CONFIG; return NULL; }
    return meteor_decode_impl(ctx, covertext, starting_context, out_msg_len, out_error);
}

/* ── utilities ────────────────────────────────────────────────────────────── */

char* meteor_syllabify_word(MeteorCtx* ctx, const char* word)
{
    if (!ctx || !word) return NULL;

    int    count;
    char** syls = syllabify_word(ctx->syl, word, &count);
    if (!syls || count == 0) { syllabify_word_free(syls, count); return NULL; }

    /* join with middle-dot U+00B7 (\xc2\xb7 in UTF-8) */
    size_t total = 0;
    for (int i = 0; i < count; i++) total += strlen(syls[i]);
    total += (size_t)(count - 1) * 2 + 1; /* 2 bytes per \xc2\xb7 separator */

    char* result = (char*)malloc(total);
    if (!result) { syllabify_word_free(syls, count); return NULL; }

    char* p = result;
    for (int i = 0; i < count; i++) {
        if (i > 0) { *p++ = '\xc2'; *p++ = '\xb7'; }
        size_t slen = strlen(syls[i]);
        memcpy(p, syls[i], slen);
        p += slen;
    }
    *p = '\0';

    syllabify_word_free(syls, count);
    return result;
}

int meteor_llm_health(MeteorCtx* ctx)
{
    if (!ctx) return 0;
    return llm_client_health(ctx->llm);
}

void meteor_free(void* ptr)
{
    free(ptr);
}
