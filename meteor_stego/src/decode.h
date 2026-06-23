#pragma once

#include "../include/meteor.h"

struct MeteorCtx;

/*
 * Full decode pipeline.
 * Returns heap-allocated message bytes on success, NULL on failure.
 * *out_msg_len is set to the number of recovered bytes.
 * *out_error is set to a METEOR_ERR_* code.
 */
uint8_t* meteor_decode_impl(struct MeteorCtx* ctx,
                             const char*       covertext,
                             const char*       starting_context,
                             size_t*           out_msg_len,
                             int*              out_error);
