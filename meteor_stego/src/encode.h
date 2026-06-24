#pragma once

#include "../include/meteor.h"

/* Forward declaration — MeteorCtx is defined in meteor.c */
struct MeteorCtx;

/*
 * Full encode pipeline.
 * Returns heap-allocated covertext on success, NULL on failure.
 * *out_error is set to a METEOR_ERR_* code.
 */
char* meteor_encode_impl(struct MeteorCtx* ctx,
                         const uint8_t*    message,
                         size_t            msg_len,
                         const char*       starting_context,
                         int*              out_error);
