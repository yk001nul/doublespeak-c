#include "doublespeak.h"
#include "../include/meteor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DOUBLESPEAK_DEFAULT_URL   "http://127.0.0.1:8080"
#define DOUBLESPEAK_DEFAULT_STYLE 1

int doublespeak_parse_args(int argc, char** argv, DoublespeakArgs* out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->style_index = DOUBLESPEAK_DEFAULT_STYLE;

    if (argc < 4) return -1; /* prog + message + context + passphrase, minimum */

    int idx = 1;
    out->message_arg = argv[idx++];

    /* -f / -d may each appear at most once, in either order, right after
       the message argument. */
    int seen_f = 0, seen_d = 0;
    while (idx < argc &&
           (strcmp(argv[idx], "-f") == 0 || strcmp(argv[idx], "-d") == 0)) {
        if (strcmp(argv[idx], "-f") == 0) {
            if (seen_f) return -1;
            seen_f = 1;
            out->is_file = 1;
        } else {
            if (seen_d) return -1;
            seen_d = 1;
            out->is_decode = 1;
        }
        idx++;
    }

    if (idx >= argc) return -1;
    out->context = argv[idx++];
    if (idx >= argc) return -1;
    out->passphrase = argv[idx++];

    /* Remaining optional args are strictly positional: style_index, then
       outputpath, then URL — none may be skipped independently. */
    if (idx < argc) {
        const char* s = argv[idx];
        char* end = NULL;
        long  v   = strtol(s, &end, 10);
        if (s[0] == '\0' || *end != '\0' || v < 1 || v > 4) return -1;
        out->style_index = (int)v;
        idx++;
    }
    if (idx < argc) {
        out->output_path = argv[idx++];
    }
    if (idx < argc) {
        out->llm_url = argv[idx++];
    }
    if (idx < argc) return -1; /* too many arguments */

    return 0;
}

void doublespeak_print_usage(const char* prog_name)
{
    const char* name = (prog_name && prog_name[0]) ? prog_name : "doublespeak";
    printf(
        "Usage: %s message [-f] [-d] context passphrase [style_index] [outputpath] [URL]\n"
        "\n"
        "  message      Text to encode, or (with -f) a path to a file containing it.\n"
        "               Under -d this is the covertext to decode instead.\n"
        "  -f           Treat 'message' as a filepath and read its contents.\n"
        "  -d           Decode instead of encode (default: encode).\n"
        "  context      Starting context / topic shared between encoder and decoder.\n"
        "  passphrase   Shared secret used to derive the encryption key.\n"
        "  style_index  1-4, selects the covertext style (default: 1).\n"
        "                 1 = informal chat   2 = formal email\n"
        "                 3 = casual blog     4 = news article\n"
        "  outputpath   Write the result to this file instead of stdout.\n"
        "  URL          llama-server URL (default: %s).\n",
        name, DOUBLESPEAK_DEFAULT_URL);
}

const char* doublespeak_error_string(int error_code)
{
    switch (error_code) {
        case METEOR_OK:           return "no error";
        case METEOR_ERR_CONFIG:   return "invalid configuration";
        case METEOR_ERR_LLM:      return "LLM server unreachable or returned an invalid response";
        case METEOR_ERR_CAPACITY: return "message too long for the configured step budget";
        case METEOR_ERR_DECODE:   return "syllabification or distribution mismatch during decode";
        case METEOR_ERR_DICT:     return "hyphenation dictionary not found";
        case METEOR_ERR_OOM:      return "memory allocation failed";
        case METEOR_ERR_TIMEOUT:  return "LLM call timed out";
        case METEOR_ERR_CRYPTO:   return "libsodium initialization or key derivation failed";
        default:                  return "unknown error";
    }
}

/* Reads the whole file at path into a heap buffer. For text mode (decode's
   covertext argument), the buffer is NUL-terminated at [size]; encode's raw
   message bytes don't need that but getting it for free is harmless. Returns
   NULL on any I/O failure. */
static uint8_t* read_file_all(const char* path, size_t* out_size)
{
    FILE* fp = fopen(path, "rb");
    if (!fp) return NULL;

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long size = ftell(fp);
    if (size < 0) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }

    uint8_t* buf = (uint8_t*)malloc((size_t)size + 1);
    if (!buf) { fclose(fp); return NULL; }

    size_t read = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    if (read != (size_t)size) { free(buf); return NULL; }

    buf[size] = '\0';
    *out_size = (size_t)size;
    return buf;
}

/* Live rolling progress for encode: there's no reliable way to predict total
   step count or per-step latency ahead of time (both depend on the live LLM
   probability distributions at each step, not just message size), so instead
   of a single upfront ETA, print an estimate that refines as real steps
   complete. Everything here goes to stderr so stdout/output files stay clean
   for the actual covertext. */
typedef struct {
    time_t start_time;
} DoublespeakProgress;

static void doublespeak_progress_cb(void* userdata, int step, int bits_done, int total_bits)
{
    DoublespeakProgress* p = (DoublespeakProgress*)userdata;
    time_t   now     = time(NULL);
    long     elapsed = (long)difftime(now, p->start_time);

    fprintf(stderr, "[doublespeak] step %d: %d/%d bits, elapsed %lds",
            step, bits_done, total_bits, elapsed);

    if (bits_done > 0 && total_bits > bits_done) {
        double rate    = (double)elapsed / (double)bits_done;
        long   eta_sec = (long)((total_bits - bits_done) * rate);
        time_t finish  = now + (time_t)eta_sec;
        struct tm* ft  = localtime(&finish);
        char finish_buf[16] = "?";
        if (ft) strftime(finish_buf, sizeof(finish_buf), "%H:%M:%S", ft);
        fprintf(stderr, ", est. ~%lds remaining (finish ~%s)", eta_sec, finish_buf);
    }
    fprintf(stderr, "\n");
}

static int write_output(const uint8_t* data, size_t len,
                        const char* output_path, int add_trailing_newline)
{
    if (output_path) {
        FILE* fp = fopen(output_path, "wb");
        if (!fp) {
            fprintf(stderr, "doublespeak: cannot open '%s' for writing\n", output_path);
            return -1;
        }
        size_t written = fwrite(data, 1, len, fp);
        fclose(fp);
        if (written != len) {
            fprintf(stderr, "doublespeak: short write to '%s'\n", output_path);
            return -1;
        }
        return 0;
    }

    fwrite(data, 1, len, stdout);
    if (add_trailing_newline) fputc('\n', stdout);
    return 0;
}

int doublespeak_run(const DoublespeakArgs* args)
{
    if (!args || !args->message_arg || !args->context || !args->passphrase)
        return -1;

    uint8_t* file_buf  = NULL;
    size_t   file_size = 0;
    const uint8_t* msg_bytes;
    size_t         msg_len;

    if (args->is_file) {
        file_buf = read_file_all(args->message_arg, &file_size);
        if (!file_buf) {
            fprintf(stderr, "doublespeak: cannot read file '%s'\n", args->message_arg);
            return -1;
        }
        msg_bytes = file_buf;
        msg_len   = file_size;
    } else {
        msg_bytes = (const uint8_t*)args->message_arg;
        msg_len   = strlen(args->message_arg);
    }

    MeteorConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.key_raw        = NULL;
    cfg.key_input      = (const uint8_t*)args->passphrase;
    cfg.key_input_len  = strlen(args->passphrase);
    cfg.salt           = NULL;  /* no salt argument in this CLI; NULL = zero salt */
    cfg.salt_len       = 0;
    cfg.beta           = 3;
    cfg.num_candidates = 8;
    cfg.llm_url        = args->llm_url ? args->llm_url : DOUBLESPEAK_DEFAULT_URL;
    cfg.hyphen_dict    = NULL;
    cfg.max_steps      = 256;
    cfg.llm_timeout_ms = 30000;
    cfg.style          = (MeteorStyle)args->style_index;

    MeteorCtx* ctx = meteor_create(&cfg);
    if (!ctx) {
        free(file_buf);
        return METEOR_ERR_CONFIG;
    }

    int result = METEOR_OK;

    if (!args->is_decode) {
        DoublespeakProgress prog = { time(NULL) };
        struct tm* st = localtime(&prog.start_time);
        char start_buf[16] = "?";
        if (st) strftime(start_buf, sizeof(start_buf), "%H:%M:%S", st);
        fprintf(stderr, "[doublespeak] encoding started %s, message=%zu bytes, max_steps=%d\n",
                start_buf, msg_len, cfg.max_steps);

        int err = METEOR_OK;
        char* covertext = meteor_encode_ex(ctx, msg_bytes, msg_len, args->context,
                                           doublespeak_progress_cb, &prog, &err);
        if (err != METEOR_OK || !covertext) {
            result = (err != METEOR_OK) ? err : METEOR_ERR_LLM;
        } else {
            if (write_output((const uint8_t*)covertext, strlen(covertext),
                             args->output_path, 1) != 0) {
                result = -1;
            }
        }
        meteor_free(covertext);
    } else {
        /* meteor_decode() takes a NUL-terminated covertext string; msg_bytes
           is already NUL-terminated whether it came from argv or
           read_file_all(). */
        int    err = METEOR_OK;
        size_t rec_len = 0;
        uint8_t* recovered = meteor_decode(ctx, (const char*)msg_bytes,
                                           args->context, &rec_len, &err);
        if (err != METEOR_OK || !recovered) {
            result = (err != METEOR_OK) ? err : METEOR_ERR_DECODE;
        } else {
            if (write_output(recovered, rec_len, args->output_path, 1) != 0) {
                result = -1;
            }
        }
        meteor_free(recovered);
    }

    meteor_destroy(ctx);
    free(file_buf);
    return result;
}
