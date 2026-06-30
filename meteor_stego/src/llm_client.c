/*
 * LLM client: HTTP POST to llama-server, JSON parsing via cJSON.
 *
 * HTTP backend selected at compile time:
 *   METEOR_HTTP_WINHTTP  – Windows-native WinHTTP (no libcurl needed on Windows)
 *   default              – libcurl (cross-platform)
 */
#include "llm_client.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <cJSON.h>

/* ── grow-buffer helper (used by both backends) ───────────────────────────── */

typedef struct {
    char*  buf;
    size_t len;
    size_t cap;
} GrowBuf;

static int growbuf_append(GrowBuf* g, const char* data, size_t n)
{
    size_t need = g->len + n + 1;
    if (need > g->cap) {
        size_t nc = g->cap ? g->cap * 2 : 4096;
        while (nc < need) nc *= 2;
        char* tmp = (char*)realloc(g->buf, nc);
        if (!tmp) return -1;
        g->buf = tmp;
        g->cap = nc;
    }
    memcpy(g->buf + g->len, data, n);
    g->len += n;
    g->buf[g->len] = '\0';
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * BACKEND: WinHTTP
 * ═══════════════════════════════════════════════════════════════════════════ */
#ifdef METEOR_HTTP_WINHTTP

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

struct LLMClientImpl {
    wchar_t host[256];
    wchar_t path_prefix[64];   /* "/completion" etc. */
    INTERNET_PORT port;
    int timeout_ms;
    int max_candidates;
};

static void parse_url(const char* url, wchar_t* host, size_t hlen,
                      INTERNET_PORT* port_out, wchar_t* path, size_t plen)
{
    /* url = "http://127.0.0.1:8080" or "http://host:port" */
    const char* p = url;
    if (strncmp(p, "http://",  7) == 0) p += 7;
    if (strncmp(p, "https://", 8) == 0) p += 8;

    const char* colon = strchr(p, ':');
    const char* slash = strchr(p, '/');

    INTERNET_PORT port = 8080;
    char host_c[256] = {0};

    if (colon && (!slash || colon < slash)) {
        size_t hl = (size_t)(colon - p);
        if (hl >= sizeof(host_c)) hl = sizeof(host_c) - 1;
        memcpy(host_c, p, hl);
        port = (INTERNET_PORT)atoi(colon + 1);
    } else {
        size_t hl = slash ? (size_t)(slash - p) : strlen(p);
        if (hl >= sizeof(host_c)) hl = sizeof(host_c) - 1;
        memcpy(host_c, p, hl);
    }

    MultiByteToWideChar(CP_UTF8, 0, host_c, -1, host, (int)hlen);
    *port_out = port;
    if (path) swprintf(path, plen, L"%s", L"");
}

LLMClient* llm_client_create(const char* base_url, int max_candidates, int timeout_ms)
{
    LLMClient* c = (LLMClient*)calloc(1, sizeof(LLMClient));
    if (!c) return NULL;
    strncpy(c->base_url, base_url ? base_url : "http://127.0.0.1:8080",
            sizeof(c->base_url) - 1);
    c->max_candidates = max_candidates > 0 ? max_candidates : 6;
    c->timeout_ms     = timeout_ms > 0     ? timeout_ms     : 30000;
    c->curl_handle    = NULL; /* unused in WinHTTP backend */

    struct LLMClientImpl* impl = (struct LLMClientImpl*)calloc(1, sizeof(struct LLMClientImpl));
    if (!impl) { free(c); return NULL; }
    parse_url(base_url, impl->host, 256, &impl->port, NULL, 0);
    impl->timeout_ms     = c->timeout_ms;
    impl->max_candidates = c->max_candidates;
    c->curl_handle = impl; /* store impl pointer here */
    return c;
}

void llm_client_destroy(LLMClient* client)
{
    if (!client) return;
    free(client->curl_handle);
    free(client);
}

static char* winhttp_post(LLMClient* client, const char* path_suffix, const char* body)
{
    struct LLMClientImpl* impl = (struct LLMClientImpl*)client->curl_handle;

    HINTERNET hSession = WinHttpOpen(L"meteor-stego/1.0",
                                     WINHTTP_ACCESS_TYPE_NO_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return NULL;

    HINTERNET hConnect = WinHttpConnect(hSession, impl->host, impl->port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return NULL; }

    wchar_t wpath[256];
    MultiByteToWideChar(CP_UTF8, 0, path_suffix, -1, wpath, 256);

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", wpath,
                                            NULL, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return NULL;
    }

    DWORD conn_timeout = (DWORD)impl->timeout_ms;
    /* llama-server holds the connection open until generation completes, so the
       body-receive timeout must cover the full generation time (can exceed 60 s
       at --threads 1).  Keep connect/send short to detect downed servers fast.
       WinHttpQueryDataAvailable ignores WINHTTP_OPTION_RECEIVE_TIMEOUT per MSDN;
       use WinHttpReadData directly so the 300 s deadline is actually enforced. */
    WinHttpSetTimeouts(hRequest, conn_timeout, conn_timeout, conn_timeout, 300000);

    BOOL sent = WinHttpSendRequest(hRequest,
                                   L"Content-Type: application/json\r\n",
                                   (DWORD)-1L,
                                   (LPVOID)body, (DWORD)strlen(body),
                                   (DWORD)strlen(body), 0);
    if (!sent || !WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return NULL;
    }

    GrowBuf gb     = {0};
    char    chunk[4096];
    DWORD   bytes_read;
    do {
        bytes_read = 0;
        if (!WinHttpReadData(hRequest, chunk, sizeof(chunk), &bytes_read)) break;
        if (bytes_read > 0) growbuf_append(&gb, chunk, bytes_read);
    } while (bytes_read > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return gb.buf; /* caller frees */
}

static int winhttp_health(LLMClient* client)
{
    struct LLMClientImpl* impl = (struct LLMClientImpl*)client->curl_handle;
    HINTERNET hSession = WinHttpOpen(L"meteor-stego/1.0",
                                     WINHTTP_ACCESS_TYPE_NO_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return 0;
    HINTERNET hConnect = WinHttpConnect(hSession, impl->host, impl->port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return 0; }
    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", L"/health",
                                        NULL, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    int ok = 0;
    if (hReq) {
        WinHttpSetTimeouts(hReq, 3000, 3000, 3000, 3000);
        if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(hReq, NULL)) {
            DWORD status = 0, sz = sizeof(status);
            WinHttpQueryHeaders(hReq,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                WINHTTP_NO_HEADER_INDEX);
            ok = (status == 200);
        }
        WinHttpCloseHandle(hReq);
    }
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

#define HTTP_POST(client, path, body)  winhttp_post((client), (path), (body))
#define HTTP_HEALTH(client)            winhttp_health((client))

/* ═══════════════════════════════════════════════════════════════════════════
 * BACKEND: libcurl
 * ═══════════════════════════════════════════════════════════════════════════ */
#else /* default: libcurl */

#include <curl/curl.h>

static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    size_t    n  = size * nmemb;
    GrowBuf*  gb = (GrowBuf*)userdata;
    return growbuf_append(gb, ptr, n) == 0 ? n : 0;
}

LLMClient* llm_client_create(const char* base_url, int max_candidates, int timeout_ms)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    LLMClient* c = (LLMClient*)calloc(1, sizeof(LLMClient));
    if (!c) return NULL;
    strncpy(c->base_url, base_url ? base_url : "http://127.0.0.1:8080",
            sizeof(c->base_url) - 1);
    c->max_candidates = max_candidates > 0 ? max_candidates : 6;
    c->timeout_ms     = timeout_ms > 0     ? timeout_ms     : 30000;
    c->curl_handle    = curl_easy_init();
    if (!c->curl_handle) { free(c); return NULL; }
    return c;
}

void llm_client_destroy(LLMClient* client)
{
    if (!client) return;
    if (client->curl_handle) curl_easy_cleanup((CURL*)client->curl_handle);
    free(client);
    curl_global_cleanup();
}

static char* curl_post(LLMClient* client, const char* path_suffix, const char* body)
{
    CURL* curl = (CURL*)client->curl_handle;
    char url[320];
    snprintf(url, sizeof(url), "%s%s", client->base_url, path_suffix);

    GrowBuf gb = {0};
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_POST,           1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &gb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,     (long)client->timeout_ms);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);

    if (rc != CURLE_OK) {
        fprintf(stderr, "meteor: curl error: %s\n", curl_easy_strerror(rc));
        free(gb.buf);
        return NULL;
    }
    return gb.buf;
}

static int curl_health(LLMClient* client)
{
    CURL* curl = (CURL*)client->curl_handle;
    char url[320];
    snprintf(url, sizeof(url), "%s/health", client->base_url);

    GrowBuf gb = {0};
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &gb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,    3000L);

    CURLcode rc = curl_easy_perform(curl);
    free(gb.buf);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    return (rc == CURLE_OK && http_code == 200) ? 1 : 0;
}

#define HTTP_POST(client, path, body)  curl_post((client), (path), (body))
#define HTTP_HEALTH(client)            curl_health((client))

#endif /* METEOR_HTTP_WINHTTP / libcurl */

/* ═══════════════════════════════════════════════════════════════════════════
 * SHARED: prompt building, JSON parsing, public API
 * ═══════════════════════════════════════════════════════════════════════════ */

/* New-word grammar: keys are plain lowercase syllables only (no EOW at word start). */
static const char* NEW_WORD_GRAMMAR =
    "root   ::= \"{\" ws pair (ws \",\" ws pair)* ws \"}\"\n"
    "pair   ::= \"\\\"\" [a-z]+ \"\\\"\" ws \":\" ws number\n"
    "number ::= \"-\"? [0-9]+ (\".\" [0-9]+)?\n"
    "ws     ::= [ \\t\\n]*\n";

/* Continuation grammar: one-or-more syllable pairs, then the EOW pair "·" is
 * mandatory at the end.  This guarantees the model always emits an EOW
 * probability so words cannot grow without bound. */
static const char* CONTINUATION_GRAMMAR =
    "root     ::= \"{\" ws syl-pair (ws \",\" ws syl-pair)* ws \",\" ws eow-pair ws \"}\"\n"
    "syl-pair ::= \"\\\"\" [a-z]+ \"\\\"\" ws \":\" ws number\n"
    "eow-pair ::= \"\\\"\xc2\xb7\\\"\" ws \":\" ws number\n"
    "number   ::= \"-\"? [0-9]+ (\".\" [0-9]+)?\n"
    "ws       ::= [ \\t\\n]*\n";

static const char* style_to_str(int style)
{
    switch (style) {
        case 1: return "informal mobile chat (e.g. WhatsApp or SMS)";
        case 2: return "formal business email";
        case 3: return "casual first-person blog post";
        case 4: return "neutral third-person news article";
        default: return NULL;
    }
}

const char* llm_client_style_seed(int style)
{
    switch (style) {
        case 1: return "I like";        /* INFORMAL_CHAT — next words: burgers/this/eating/good/… */
        case 2: return "I am";          /* FORMAL_EMAIL  — next words: writing/pleased/happy/… */
        case 3: return "I love";        /* CASUAL_BLOG   — next words: burgers/eating/how/… */
        case 4: return "Scientists say"; /* NEWS_ARTICLE — next words: that/the/global/temperatures/… */
        default: return NULL;
    }
}

char* llm_client_build_preamble(int style, const char* topic)
{
    const char* sname = style_to_str(style);
    if (!sname || !topic || !*topic) return NULL;
    size_t n = strlen(topic) + strlen(sname) + 128;
    char* buf = (char*)malloc(n);
    if (!buf) return NULL;
    snprintf(buf, n,
        "Background idea: \"%s\"\n"
        "Style: %s\n"
        "Express this idea naturally — do NOT echo the background words literally.\n"
        "---\n",
        topic, sname);
    return buf;
}

static char* build_new_word_prompt(const char* preamble, const char* ctx, int n)
{
    size_t pre_len = preamble ? strlen(preamble) : 0;
    size_t ctx_len = ctx     ? strlen(ctx)     : 0;
    size_t buf_size = pre_len + ctx_len + 256;
    char* buf = (char*)malloc(buf_size);
    if (!buf) return NULL;
    if (preamble) {
        snprintf(buf, buf_size,
            "%s"
            "Text so far: \"%s\"\n"
            "You are continuing this text in the given style about the given topic.\n"
            "Provide the %d most natural first syllables for the next word.\n"
            "Return ONLY a JSON object like: {\"the\": 0.4, \"in\": 0.3, \"re\": 0.2, \"pro\": 0.1} — probs sum to 1.0.",
            preamble, ctx, n);
    } else {
        snprintf(buf, buf_size,
            "Text so far: \"%s\"\n"
            "You are generating the next word one syllable at a time.\n"
            "Provide the %d most natural first syllables for the next word.\n"
            "Return ONLY a JSON object like: {\"the\": 0.4, \"in\": 0.3, \"re\": 0.2, \"pro\": 0.1} — probs sum to 1.0.",
            ctx, n);
    }
    return buf;
}

static char* build_continuation_prompt(const char* preamble, const char* ctx,
                                        const char* partial, int n)
{
    size_t pre_len = preamble ? strlen(preamble) : 0;
    size_t ctx_len = ctx     ? strlen(ctx)     : 0;
    size_t par_len = partial ? strlen(partial) : 0;
    size_t buf_size = pre_len + ctx_len + par_len * 2 + 256;
    char* buf = (char*)malloc(buf_size);
    if (!buf) return NULL;
    if (preamble) {
        snprintf(buf, buf_size,
            "%s"
            "Text so far: \"%s\"\n"
            "Word being built: \"%s\"\n"
            "You are continuing this text in the given style about the given topic.\n"
            "Provide %d natural continuation syllables plus \"\xc2\xb7\" (end-of-word).\n"
            "Higher prob for \"\xc2\xb7\" if \"%s\" is already a natural word.\n"
            "Return ONLY a JSON object like: {\"\xc2\xb7\": 0.5, \"tion\": 0.3, \"ing\": 0.2} — probs sum to 1.0.",
            preamble, ctx, partial, n - 1, partial);
    } else {
        snprintf(buf, buf_size,
            "Text so far: \"%s\"\n"
            "Word being built: \"%s\"\n"
            "Provide %d natural continuation syllables plus \"\xc2\xb7\" (end-of-word).\n"
            "Higher prob for \"\xc2\xb7\" if \"%s\" is already a natural word.\n"
            "Return ONLY a JSON object like: {\"\xc2\xb7\": 0.5, \"tion\": 0.3, \"ing\": 0.2} — probs sum to 1.0.",
            ctx, partial, n - 1, partial);
    }
    return buf;
}

static void map_eow(char* text)
{
    if (strcmp(text, "\xc2\xb7") == 0)
        strcpy(text, "\x01");
}

static LLMResponse* parse_llm_response(const char* raw_json, int max_candidates)
{
    cJSON* env = cJSON_Parse(raw_json);
    if (!env) return NULL;

    const char* content = NULL;
    cJSON* ci = cJSON_GetObjectItemCaseSensitive(env, "content");
    if (ci && cJSON_IsString(ci)) content = ci->valuestring;
    if (!content) { cJSON_Delete(env); return NULL; }

    cJSON* obj = cJSON_Parse(content);
    cJSON_Delete(env);
    if (!obj) return NULL;

    int n = cJSON_GetArraySize(obj);
    if (n <= 0) { cJSON_Delete(obj); return NULL; }
    if (n > max_candidates) n = max_candidates;

    LLMResponse* resp = (LLMResponse*)calloc(1, sizeof(LLMResponse));
    resp->candidates  = (LLMCandidate*)calloc((size_t)n, sizeof(LLMCandidate));
    resp->count       = 0;

    float sum = 0.0f;
    cJSON* item; int i = 0;
    cJSON_ArrayForEach(item, obj) {
        if (i >= n) break;
        if (!cJSON_IsNumber(item)) continue;
        float p = (float)item->valuedouble;
        if (p < 0.0f) p = 0.0f;

        char text[64];
        strncpy(text, item->string, 63);
        text[63] = '\0';
        map_eow(text);

        /* skip duplicate keys — JSON with repeated keys causes encode/decode divergence */
        int dup = 0;
        for (int j = 0; j < i; j++) {
            if (strcmp(resp->candidates[j].text, text) == 0) { dup = 1; break; }
        }
        if (dup) continue;

        strncpy(resp->candidates[i].text, text, 63);
        resp->candidates[i].text[63] = '\0';
        resp->candidates[i].prob = p;
        sum += p;
        i++;
        resp->count = i;
    }
    cJSON_Delete(obj);

    if (sum > 0.0f)
        for (int j = 0; j < resp->count; j++)
            resp->candidates[j].prob /= sum;

    return resp;
}

static char* build_word_prompt(const char* preamble, const char* ctx, int n,
                               const char* blacklist)
{
    size_t pre_len    = preamble   ? strlen(preamble)   : 0;
    size_t ctx_len    = ctx && ctx[0] ? strlen(ctx)     : 0;
    size_t bl_len     = blacklist && blacklist[0] ? strlen(blacklist) : 0;
    size_t buf_size   = pre_len + ctx_len + bl_len + 640;
    char*  buf        = (char*)malloc(buf_size);
    if (!buf) return NULL;

    const char* instruction = ctx_len > 0
        ? "Continue the sentence below naturally. Use meaningful nouns, verbs, and adjectives — do NOT use generic filler adverbs or repeat words from the background."
        : "Begin a natural sentence expressing the background idea. Use a meaningful content word — do NOT copy words from the background.";

    /* blacklist clause — empty string when no prior word */
    char bl_clause[96] = {0};
    if (bl_len > 0)
        snprintf(bl_clause, sizeof(bl_clause),
                 "\nDo NOT suggest \"%s\" (just used — vary the vocabulary).", blacklist);

    if (preamble) {
        snprintf(buf, buf_size,
            "%s"
            "%s%s\n"
            "Sentence so far: \"%s\"\n"
            "Provide the %d most natural next words (nouns, verbs, adjectives preferred).\n"
            "Return ONLY a JSON object like: {\"enjoying\": 0.4, \"craving\": 0.3, \"fantastic\": 0.2, \"wonderful\": 0.1} — probs sum to 1.0.",
            preamble, instruction, bl_clause, ctx ? ctx : "", n);
    } else {
        snprintf(buf, buf_size,
            "Write a natural sentence. Sentence so far: \"%s\"\n"
            "%s%s\n"
            "Provide the %d most probable next words.\n"
            "Return ONLY a JSON object — probs sum to 1.0.",
            ctx ? ctx : "", instruction, bl_clause, n);
    }
    return buf;
}

static const char* FALLBACK_SYLLABLES[] = {
    "the", "in", "a", "re", "pro", "con", "de", "ex", "un", "be",
    "per", "dis", "over", "out", "sub", "pre", "inter", "mis", "non", "bi"
};
#define FALLBACK_SYLLABLES_COUNT 20

static const char* FALLBACK_WORDS[] = {
    "the", "is", "a", "and", "of", "it", "to", "in", "that", "have",
    "for", "on", "are", "with", "as", "at", "be", "this", "was", "but"
};
#define FALLBACK_WORDS_COUNT 20

static LLMResponse* uniform_word_fallback(int n)
{
    LLMResponse* resp = (LLMResponse*)calloc(1, sizeof(LLMResponse));
    resp->candidates  = (LLMCandidate*)calloc((size_t)n, sizeof(LLMCandidate));
    resp->count       = n;
    float p = 1.0f / (float)n;
    for (int i = 0; i < n; i++) {
        strncpy(resp->candidates[i].text,
                FALLBACK_WORDS[i % FALLBACK_WORDS_COUNT], 63);
        resp->candidates[i].text[63] = '\0';
        resp->candidates[i].prob = p;
    }
    return resp;
}

static LLMResponse* uniform_fallback(int n)
{
    LLMResponse* resp = (LLMResponse*)calloc(1, sizeof(LLMResponse));
    resp->candidates  = (LLMCandidate*)calloc((size_t)n, sizeof(LLMCandidate));
    resp->count       = n;
    float p = 1.0f / (float)n;
    for (int i = 0; i < n; i++) {
        strncpy(resp->candidates[i].text,
                FALLBACK_SYLLABLES[i % FALLBACK_SYLLABLES_COUNT], 63);
        resp->candidates[i].text[63] = '\0';
        resp->candidates[i].prob = p;
    }
    return resp;
}

/* ── public API ─────────────────────────────────────────────────────────── */

LLMResponse* llm_client_get_syllable_dist(LLMClient*  client,
                                           const char* preamble,
                                           const char* full_context,
                                           const char* partial_word,
                                           int         is_new_word)
{
    char* prompt = is_new_word
        ? build_new_word_prompt(preamble, full_context, client->max_candidates)
        : build_continuation_prompt(preamble, full_context, partial_word, client->max_candidates);
    if (!prompt) return NULL;

    cJSON* req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "prompt",      prompt);
    cJSON_AddStringToObject(req, "grammar",     is_new_word ? NEW_WORD_GRAMMAR : CONTINUATION_GRAMMAR);
    cJSON_AddNumberToObject(req, "n_predict",   128);
    cJSON_AddNumberToObject(req, "temperature", 0.0);
    cJSON_AddNumberToObject(req, "seed",        42);
    cJSON_AddBoolToObject  (req, "stream",      0);
    char* body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    free(prompt);
    if (!body) return NULL;

    char* raw = HTTP_POST(client, "/completion", body);
    free(body);

    if (!raw) return uniform_fallback(client->max_candidates);

    LLMResponse* resp = parse_llm_response(raw, client->max_candidates);
    free(raw);

    if (!resp || resp->count == 0) {
        llm_response_free(resp);
        return uniform_fallback(client->max_candidates);
    }
    return resp;
}

LLMResponse* llm_client_get_word_dist(LLMClient*  client,
                                       const char* preamble,
                                       const char* full_context,
                                       const char* blacklist_word)
{
    char* prompt = build_word_prompt(preamble, full_context, client->max_candidates,
                                     blacklist_word);
    if (!prompt) return NULL;

    cJSON* req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "prompt",      prompt);
    cJSON_AddStringToObject(req, "grammar",     NEW_WORD_GRAMMAR);
    cJSON_AddNumberToObject(req, "n_predict",   64);
    cJSON_AddNumberToObject(req, "temperature", 0.0);
    cJSON_AddNumberToObject(req, "seed",        42);
    cJSON_AddBoolToObject  (req, "stream",      0);
    char* body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    free(prompt);
    if (!body) return NULL;

    char* raw = HTTP_POST(client, "/completion", body);
    free(body);

    if (!raw) return uniform_word_fallback(client->max_candidates);

    LLMResponse* resp = parse_llm_response(raw, client->max_candidates);
    free(raw);

    if (!resp || resp->count == 0) {
        llm_response_free(resp);
        return uniform_word_fallback(client->max_candidates);
    }
    return resp;
}

void llm_response_free(LLMResponse* resp)
{
    if (!resp) return;
    free(resp->candidates);
    free(resp);
}

int llm_client_health(LLMClient* client)
{
    if (!client) return 0;
    return HTTP_HEALTH(client);
}
