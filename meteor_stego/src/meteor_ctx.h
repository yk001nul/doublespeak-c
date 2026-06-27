/*
 * Internal header: MeteorCtx struct definition.
 * Only included by meteor.c, encode.c, decode.c.
 */
#pragma once

#include "../include/meteor.h"
#include "prng.h"
#include "llm_client.h"
#include "syllabifier.h"

struct MeteorCtx {
    uint8_t  key[32];            /* derived 256-bit key */
    uint8_t  salt[32];
    size_t   salt_len;

    int      beta;
    int      num_candidates;
    int      max_steps;
    int      llm_timeout_ms;
    int      num_threads;         /* resolved value, always >= 1 */
    char     llm_url[256];
    char     hyphen_dict_path[512];

    LLMClient* llm;
    SylCtx*    syl;
};
