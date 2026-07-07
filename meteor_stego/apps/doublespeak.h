#ifndef DOUBLESPEAK_H
#define DOUBLESPEAK_H

#include <stddef.h>

/*
 * Parsed form of:
 *   doublespeak message [-f] [-d] context passphrase [style_index] [outputpath] [URL]
 *
 * message_arg is either the literal message/covertext text, or (if is_file) a
 * filepath whose contents should be used instead. style_index is already
 * validated to be in [1,4] and maps directly onto MeteorStyle's non-zero
 * values. output_path / llm_url are NULL when omitted (stdout / the default
 * server URL respectively).
 */
typedef struct {
    const char* message_arg;
    int         is_file;
    int         is_decode;
    const char* context;
    const char* passphrase;
    int         style_index;
    const char* output_path;
    const char* llm_url;
} DoublespeakArgs;

/*
 * Parses argv[1..argc-1] into *out. Returns 0 on success, -1 on any parse
 * error (missing required argument, invalid/out-of-range style_index, or
 * extra trailing arguments) — does not print anything itself.
 */
int doublespeak_parse_args(int argc, char** argv, DoublespeakArgs* out);

/* Prints the argument format / usage guide to stdout. */
void doublespeak_print_usage(const char* prog_name);

/*
 * Runs the encode or decode operation described by args, writing the result
 * to stdout or args->output_path. Returns METEOR_OK (0) on success, another
 * METEOR_ERR_* code from meteor.h on library failure, or -1 on a
 * doublespeak-level error (e.g. file I/O), in which case a message has
 * already been printed to stderr.
 */
int doublespeak_run(const DoublespeakArgs* args);

/* Short human-readable description of a METEOR_ERR_* (or METEOR_OK) code. */
const char* doublespeak_error_string(int error_code);

#endif /* DOUBLESPEAK_H */
