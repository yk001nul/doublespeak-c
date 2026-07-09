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
    /* Persistent session + connection reused across every POST (HTTP
       keep-alive). Opening/closing these per request added a TCP connect +
       teardown to every LLM step; llama-server keeps the connection alive, so
       we open once and only allocate a fresh request handle per call. Both are
       lazily (re)opened by winhttp_ensure_conn() and torn down on destroy. */
    HINTERNET hSession;
    HINTERNET hConnect;
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
    impl->hSession       = NULL;
    impl->hConnect       = NULL;
    c->curl_handle = impl; /* store impl pointer here */
    return c;
}

void llm_client_destroy(LLMClient* client)
{
    if (!client) return;
    struct LLMClientImpl* impl = (struct LLMClientImpl*)client->curl_handle;
    if (impl) {
        if (impl->hConnect) WinHttpCloseHandle(impl->hConnect);
        if (impl->hSession) WinHttpCloseHandle(impl->hSession);
    }
    free(client->curl_handle);
    free(client);
}

/* Lazily open the persistent session + connection, reusing them if already
   open. Returns 0 on success, -1 on failure (handles left NULL). */
static int winhttp_ensure_conn(struct LLMClientImpl* impl)
{
    if (impl->hSession && impl->hConnect) return 0;
    if (!impl->hSession) {
        impl->hSession = WinHttpOpen(L"meteor-stego/1.0",
                                     WINHTTP_ACCESS_TYPE_NO_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
        if (!impl->hSession) return -1;
    }
    if (!impl->hConnect) {
        impl->hConnect = WinHttpConnect(impl->hSession, impl->host, impl->port, 0);
        if (!impl->hConnect) return -1;
    }
    return 0;
}

/* Drop the persistent connection (e.g. after a send/receive error) so the
   next call reopens it. The session is kept. */
static void winhttp_reset_conn(struct LLMClientImpl* impl)
{
    if (impl->hConnect) { WinHttpCloseHandle(impl->hConnect); impl->hConnect = NULL; }
}

/* One send/receive attempt over the persistent connection. Returns the
   response body (caller frees) on success; NULL on any failure, with
   *out_conn_err set to 1 if the failure looks connection-level (so the caller
   can drop + reopen the connection and retry once). */
static char* winhttp_post_once(struct LLMClientImpl* impl, const char* path_suffix,
                               const char* body, int* out_conn_err)
{
    *out_conn_err = 0;
    if (winhttp_ensure_conn(impl) != 0) { *out_conn_err = 1; return NULL; }

    wchar_t wpath[256];
    MultiByteToWideChar(CP_UTF8, 0, path_suffix, -1, wpath, 256);

    HINTERNET hRequest = WinHttpOpenRequest(impl->hConnect, L"POST", wpath,
                                            NULL, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) { *out_conn_err = 1; return NULL; }

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
        *out_conn_err = 1;   /* stale keep-alive socket — reopen and retry */
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
    return gb.buf; /* caller frees */
}

static char* winhttp_post(LLMClient* client, const char* path_suffix, const char* body)
{
    struct LLMClientImpl* impl = (struct LLMClientImpl*)client->curl_handle;

    int conn_err = 0;
    char* resp = winhttp_post_once(impl, path_suffix, body, &conn_err);
    if (!resp && conn_err) {
        /* A persistent keep-alive socket can be closed by the server between
           calls; drop it and retry once from a fresh connection. */
        winhttp_reset_conn(impl);
        resp = winhttp_post_once(impl, path_suffix, body, &conn_err);
    }
    return resp;
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
    /* Keep the TCP connection alive between steps. curl_easy_reset() above wipes
       per-transfer options but the easy handle's connection cache survives, so
       the socket to llama-server is reused rather than reconnected each call. */
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE,  1L);

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

/* Opening-step grammar: same JSON shape as the old free-form phrase
 * grammar, but the phrase is forced to start with a pronoun or a
 * determiner+noun ("the team", "his car") before any other word. This is
 * the same class of fix as STYLE_QUESTION_CONNECTOR_GRAMMAR below: the
 * soft prompt instruction ("every candidate MUST start with an explicit
 * subject") was frequently ignored by the model at temp=0.0, producing
 * bare-verb sentence openers ("starts his car...", "hits the road...")
 * whenever a clause-end reset started a fresh sentence mid-covertext.
 * Subject-hood itself isn't a closed vocabulary (unlike the connector
 * case), so this can't fully enumerate valid subjects the way the
 * connector grammar does — but a bare verb is never a pronoun or a
 * determiner, so gating the first word on this set closes the loophole
 * even though it doesn't cover every grammatically valid subject (e.g. a
 * bare proper noun like "sarah" without a determiner is also excluded). */
static const char* OPENING_SUBJECT_GRAMMAR =
    "root        ::= \"{\" ws phrase-pair (ws \",\" ws phrase-pair)* ws \"}\"\n"
    "phrase-pair ::= \"\\\"\" phrase \"\\\"\" ws \":\" ws number\n"
    "phrase      ::= subject (\" \" word)+\n"
    "subject     ::= pronoun | (determiner \" \" word)\n"
    "pronoun     ::= \"he\" | \"she\" | \"it\" | \"they\" | \"we\" | \"i\" | \"you\"\n"
    "determiner  ::= \"the\" | \"a\" | \"an\" | \"this\" | \"that\" | \"his\" | \"her\" | \"their\" | \"its\"\n"
    "word        ::= [a-z]+\n"
    "number      ::= \"-\"? [0-9]+ (\".\" [0-9]+)?\n"
    "ws          ::= [ \\t\\n]*\n";

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
    size_t n = strlen(topic) + strlen(sname) + 192;
    char* buf = (char*)malloc(n);
    if (!buf) return NULL;
    snprintf(buf, n,
        "Paraphrase the sentence below as a %s.\n"
        "Keep the same meaning but use natural vocabulary for that style.\n"
        "Original: \"%s\"\n"
        "---\n",
        sname, topic);
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

    /* First step: no text generated yet; ask for a strong opening word.
       Subsequent steps: continue the paraphrase from where it left off. */
    const char* phase = ctx_len > 0
        ? "Continue the paraphrase. Use specific nouns, verbs, and details from the original — do NOT use emotional adjectives like wonderful, fantastic, or craving."
        : "Begin the paraphrase with one strong content word from the original — do NOT use emotional adjectives.";

    /* blacklist clause — suppress the word used in the previous step */
    char bl_clause[96] = {0};
    if (bl_len > 0)
        snprintf(bl_clause, sizeof(bl_clause),
                 "\nDo NOT repeat \"%s\" (just used).", blacklist);

    if (preamble) {
        snprintf(buf, buf_size,
            "%s"
            "%s%s\n"
            "Paraphrase so far: \"%s\"\n"
            "Provide the %d most probable next words.\n"
            "Return ONLY a JSON object like: {\"enjoying\": 0.4, \"craving\": 0.3, \"fantastic\": 0.2, \"wonderful\": 0.1} — probs sum to 1.0.",
            preamble, phase, bl_clause, ctx ? ctx : "", n);
    } else {
        snprintf(buf, buf_size,
            "Write a natural sentence. Sentence so far: \"%s\"\n"
            "%s%s\n"
            "Provide the %d most probable next words.\n"
            "Return ONLY a JSON object — probs sum to 1.0.",
            ctx ? ctx : "", phase, bl_clause, n);
    }
    return buf;
}

/* Per-question continuation instructions, indexed by StyleQuestion.
   Only used when ctx_len > 0 (see phase selection below) — each narrows
   the candidates to one concrete axis of variation instead of an
   open-ended "continue naturally". The opening connector named here is
   no longer just a hint: build_phrase_grammar() below forces every
   candidate's phrase to start with one of these exact words via GBNF, so
   the wording states it as a requirement rather than "typically" — the
   model has no way to produce a bare new-verb clause instead. */
static const char* STYLE_QUESTION_PHASE[STYLE_Q_COUNT] = {
    /* STYLE_Q_HOW */
    "Continue the sentence above by answering HOW the subject does this — "
    "the method, tool, or manner involved. Every candidate MUST open with "
    "\"by\", \"through\", or \"using\", followed by its next 1-4 words "
    "(\"by turning the key\"). Agree in tense and subject with the text "
    "that precedes it — do NOT switch subject or start a new sentence.",

    /* STYLE_Q_WHERE */
    "Continue the sentence above by answering WHERE this is happening — a "
    "destination, origin, or place. Every candidate MUST open with \"to\", "
    "\"toward\", \"from\", or \"at\", followed by its next 1-4 words "
    "(\"to the office\"). Agree in tense and subject with the text that "
    "precedes it — do NOT switch subject or start a new sentence.",

    /* STYLE_Q_WHO_MEET */
    "Continue the sentence above by answering WHO the subject intends to "
    "meet, involve, or work with as part of this. Every candidate MUST "
    "open with \"to meet\", \"to join\", or \"with\", followed by its next "
    "1-4 words naming a person or group (\"to meet the manager\"). Agree "
    "in tense and subject with the text that precedes it — do NOT switch "
    "subject or start a new sentence.",

    /* STYLE_Q_WHO_AVOID */
    "Continue the sentence above by answering WHO or WHAT the subject "
    "wants to avoid, delay, or steer clear of while doing this. Every "
    "candidate MUST open with \"to avoid\", \"before\", or \"while "
    "avoiding\", followed by its next 1-4 words (\"to avoid the "
    "traffic\"). Agree in tense and subject with the text that precedes "
    "it — do NOT switch subject or start a new sentence.",

    /* STYLE_Q_WHY */
    "Continue the sentence above by answering WHY the subject is doing "
    "this — the reason or goal behind it. Every candidate MUST open with "
    "\"to\", \"in order to\", or \"because\", followed by its next 1-4 "
    "words (\"to make it to the meeting\"). Agree in tense and subject "
    "with the text that precedes it — do NOT switch subject or start a "
    "new sentence.",
};

/* Connector alternatives per question, as GBNF string-literal alternation
   bodies (each entry is valid inside a "(...)" grammar group). Must stay
   in sync word-for-word with the connectors named in STYLE_QUESTION_PHASE
   above, or the prompt will describe options the grammar doesn't allow. */
static const char* STYLE_QUESTION_CONNECTOR_GRAMMAR[STYLE_Q_COUNT] = {
    /* STYLE_Q_HOW       */ "\"by\" | \"through\" | \"using\"",
    /* STYLE_Q_WHERE     */ "\"to\" | \"toward\" | \"from\" | \"at\"",
    /* STYLE_Q_WHO_MEET  */ "\"to meet\" | \"to join\" | \"with\"",
    /* STYLE_Q_WHO_AVOID */ "\"to avoid\" | \"before\" | \"while avoiding\"",
    /* STYLE_Q_WHY       */ "\"to\" | \"in order to\" | \"because\"",
};

/* Continuation-step grammar: same JSON shape as OPENING_SUBJECT_GRAMMAR, but each
   phrase is forced to start with one of the question's connector words
   (see STYLE_QUESTION_CONNECTOR_GRAMMAR), guaranteeing every candidate —
   not just "roughly half" — reads as a subordinate clause glued onto the
   sentence so far, instead of a bare new finite-verb clause. Caller frees
   with free(). Returns NULL on OOM. */
static char* build_phrase_grammar(StyleQuestion question)
{
    const char* connectors = STYLE_QUESTION_CONNECTOR_GRAMMAR[question];
    size_t buf_size = strlen(connectors) + 384;
    char*  buf      = (char*)malloc(buf_size);
    if (!buf) return NULL;
    snprintf(buf, buf_size,
        "root        ::= \"{\" ws phrase-pair (ws \",\" ws phrase-pair)* ws \"}\"\n"
        "phrase-pair ::= \"\\\"\" phrase \"\\\"\" ws \":\" ws number\n"
        "phrase      ::= connector (\" \" word)+\n"
        "connector   ::= %s\n"
        "word        ::= [a-z]+\n"
        "number      ::= \"-\"? [0-9]+ (\".\" [0-9]+)?\n"
        "ws          ::= [ \\t\\n]*\n",
        connectors);
    return buf;
}

/* ── digression: stage 1, internal question (never written to covertext) ── */
/* Axis-only (style-agnostic) — style flavor is applied in stage 2 by
   reusing llm_client_build_preamble() on the answer text, exactly like the
   main topic. Two variants per axis purely for wording variety across a
   long message. Both variants ask the model to pick its own secondary
   entity from the text so far (no explicit NER — same "let the model pick"
   approach as the rest of style mode) and answer directly, in one plain
   sentence, with no grammar constraint (this call never carries message
   bits — see llm_client_get_digression_answer()). */
static const char* DIGRESSION_QUESTION[DIGRESS_AXIS_COUNT][DIGRESS_VARIANT_COUNT] = {
    /* DIGRESS_AXIS_DESCRIBE */
    {
        "Look at the sentence you just wrote and pick a secondary object or "
        "person mentioned there (not the main subject). What does it look "
        "like, or what kind is it?",
        "From the sentence you just wrote, pick something or someone "
        "secondary (not the main subject). How would you describe its "
        "appearance or type?",
    },
    /* DIGRESS_AXIS_STATE */
    {
        "Look at the sentence you just wrote and pick a secondary object or "
        "person mentioned there (not the main subject). What is its "
        "current condition or status?",
        "From the sentence you just wrote, pick something or someone "
        "secondary (not the main subject). Is it still the same, or has "
        "something changed about it?",
    },
    /* DIGRESS_AXIS_SIGNIFICANCE */
    {
        "Look at the sentence you just wrote and pick a secondary object or "
        "person mentioned there (not the main subject). Why does it "
        "matter, or how serious is it?",
        "From the sentence you just wrote, pick something or someone "
        "secondary (not the main subject). What impact or consequence "
        "does it have?",
    },
    /* DIGRESS_AXIS_ORIGIN */
    {
        "Look at the sentence you just wrote and pick a secondary object or "
        "person mentioned there (not the main subject). How did it come "
        "about, or why does it exist?",
        "From the sentence you just wrote, pick something or someone "
        "secondary (not the main subject). What caused it or led to it?",
    },
    /* DIGRESS_AXIS_OUTCOME */
    {
        "Look at the sentence you just wrote and pick a secondary object or "
        "person mentioned there (not the main subject). What is likely to "
        "happen to it next?",
        "From the sentence you just wrote, pick something or someone "
        "secondary (not the main subject). What will be the result or "
        "next step involving it?",
    },
};

static char* build_digression_question_prompt(const char* ctx, DigressionAxis axis, int variant)
{
    const char* question = DIGRESSION_QUESTION[axis][variant];
    size_t ctx_len  = ctx && ctx[0] ? strlen(ctx) : 0;
    size_t buf_size = ctx_len + strlen(question) + 160;
    char*  buf      = (char*)malloc(buf_size);
    if (!buf) return NULL;
    snprintf(buf, buf_size,
        "Text so far: \"%s\"\n"
        "%s\n"
        "Answer in one short, plain sentence. Do not restate the question "
        "or add commentary — just give the answer.",
        ctx ? ctx : "", question);
    return buf;
}

/* Extracts and lightly sanitizes the plain-text answer from a /completion
   response whose "content" is natural language (not the JSON-candidate
   shape parse_llm_response expects): trims leading whitespace/quotes,
   collapses newlines to spaces, drops stray quote characters (so it nests
   cleanly inside llm_client_build_preamble()'s Original: "..." wrapper),
   and truncates at the first sentence-ending punctuation. Returns NULL on
   unparseable/empty input — caller falls back to a fixed answer. */
static char* parse_llm_plain_answer(const char* raw_json)
{
    cJSON* env = cJSON_Parse(raw_json);
    if (!env) return NULL;
    cJSON* ci = cJSON_GetObjectItemCaseSensitive(env, "content");
    const char* content = (ci && cJSON_IsString(ci)) ? ci->valuestring : NULL;
    if (!content) { cJSON_Delete(env); return NULL; }

    const char* start = content;
    while (*start == ' ' || *start == '\t' || *start == '\n' ||
           *start == '\r' || *start == '"')
        start++;

    char* out = (char*)malloc(strlen(start) + 1);
    if (!out) { cJSON_Delete(env); return NULL; }
    size_t oi = 0;
    for (const char* p = start; *p != '\0'; p++) {
        char c = *p;
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == '"') continue;
        out[oi++] = c;
        if (c == '.' || c == '!' || c == '?') break;
    }
    out[oi] = '\0';
    cJSON_Delete(env);

    if (out[0] == '\0') { free(out); return NULL; }
    return out;
}

/* Deterministic local fallback (no server round-trip) used if the /completion
   call fails or returns something unparseable, so both encode.c and decode.c
   still get byte-identical text to paraphrase in stage 2. */
static const char* DIGRESSION_FALLBACK_ANSWER =
    "it has changed in a small but noticeable way";

static char* dup_digression_fallback(void)
{
    size_t n = strlen(DIGRESSION_FALLBACK_ANSWER) + 1;
    char*  fb = (char*)malloc(n);
    if (fb) memcpy(fb, DIGRESSION_FALLBACK_ANSWER, n);
    return fb;
}

/* Stage 1 of a digression: asks DIGRESSION_QUESTION[axis][variant] as a
   plain, non-bit-embedding /completion call (no grammar — free text) and
   returns the answer (caller frees). Never returns NULL except on OOM —
   HTTP/parse failures fall back to a fixed deterministic string so encode
   and decode stay in lockstep even if the server hiccups. The returned
   text is meant to be fed into llm_client_build_preamble() and then the
   normal phrase-distribution/beta-bit machinery (stage 2), exactly like
   the main topic — see meteor_core.h's DigressionAxis doc comment. */
char* llm_client_get_digression_answer(LLMClient* client, const char* full_context,
                                        DigressionAxis axis, int variant)
{
    char* prompt = build_digression_question_prompt(full_context, axis, variant);
    if (!prompt) return NULL;

    cJSON* req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "prompt",      prompt);
    cJSON_AddNumberToObject(req, "n_predict",   48);
    cJSON_AddNumberToObject(req, "temperature", 0.0);
    cJSON_AddNumberToObject(req, "seed",        42);
    cJSON_AddBoolToObject  (req, "stream",      0);
    cJSON_AddBoolToObject  (req, "cache_prompt", 0);
    cJSON* stop = cJSON_CreateArray();
    cJSON_AddItemToArray(stop, cJSON_CreateString("\n"));
    cJSON_AddItemToObject(req, "stop", stop);
    char* body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    free(prompt);
    if (!body) return NULL;

    char* raw = HTTP_POST(client, "/completion", body);
    free(body);
    if (!raw) return dup_digression_fallback();

    char* answer = parse_llm_plain_answer(raw);
    free(raw);
    return answer ? answer : dup_digression_fallback();
}

static char* build_phrase_prompt(const char* preamble, const char* ctx, int n,
                                  const char* blacklist_phrases,
                                  const char* blacklist_words,
                                  const char* subject_anchor,
                                  StyleQuestion question)
{
    size_t pre_len  = preamble         ? strlen(preamble)         : 0;
    size_t ctx_len  = ctx && ctx[0]    ? strlen(ctx)              : 0;
    size_t bl_len   = blacklist_phrases && blacklist_phrases[0]
                      ? strlen(blacklist_phrases) : 0;
    size_t bw_len   = blacklist_words && blacklist_words[0]
                      ? strlen(blacklist_words) : 0;
    size_t sa_len   = subject_anchor && subject_anchor[0]
                      ? strlen(subject_anchor) : 0;
    /* 2048 covers the fixed wrapper/instruction text (phase + subj/bl/bw
       clause wording + JSON-format example) with headroom — measured at
       ~1150 bytes as of the subject-anchor + word-blacklist prompt. A
       flat 1024 was undersized here and silently truncated the trailing
       "Return ONLY a JSON object..." format example via snprintf, which
       correlated with a spike in the model failing to return parseable
       JSON (falling back to the hardcoded FALLBACK_PHRASES table). */
    size_t buf_size = pre_len + ctx_len + bl_len + bw_len + sa_len + 2048;
    char*  buf      = (char*)malloc(buf_size);
    if (!buf) return NULL;

    /* Opening step of a sentence (subject_anchor empty — either the very
       first phrase of the covertext, or the first phrase after a clause
       ended and reset the anchor): every candidate MUST open with an
       explicit subject so later steps have a grammatical anchor to agree
       with (a bare verb-phrase/prepositional opener has nothing to agree
       with, which is what caused subject-less fragments in practice). A
       digression sentence also lands here — its "preamble" argument is a
       one-off temporary preamble quoting the stage-1 answer text instead
       of the main topic (see llm_client_get_digression_answer() and its
       call sites in encode.c/decode.c), so this function itself needs no
       digression-specific branch: it just paraphrases whatever preamble
       it was given, same code path either way.
       Continuation step (subject_anchor set): must grammatically continue
       the current sentence (same subject, same tense, no restart). Keyed
       off sa_len rather than ctx_len so a second sentence mid-covertext
       also gets opening-step treatment, not just the very first phrase.
       As of OPENING_SUBJECT_GRAMMAR, the subject requirement below is
       enforced by GBNF, not just prompted — this wording states it as a
       requirement (matching the connector wording style) rather than a
       request, since the model has no way to produce a bare-verb opener
       instead. */
    const char* phase = sa_len > 0
        ? STYLE_QUESTION_PHASE[question]
        : "Provide the opening 3-6 words of the paraphrase. Every candidate MUST "
          "start with an explicit subject — a pronoun (he/she/they/it) or a "
          "determiner + noun (\"the team\", \"his car\", \"this plan\") — "
          "immediately followed by its verb. Do NOT start with a preposition, a "
          "bare verb, or a dangling phrase with no subject.";

    char bl_clause[640] = {0};
    if (bl_len > 0)
        snprintf(bl_clause, sizeof(bl_clause),
                 "\nDo NOT repeat any of these already-used phrases: %s.",
                 blacklist_phrases);

    /* Word-level blacklist catches repetition the phrase-level blacklist
       misses: a candidate can dodge the exact-phrase check while still
       reusing an individual content word from an earlier phrase (e.g.
       "rides smoothly" then "smoothly continues" a few steps later). */
    char bw_clause[640] = {0};
    if (bw_len > 0)
        snprintf(bw_clause, sizeof(bw_clause),
                 "\nAlso avoid reusing these specific words in any candidate, "
                 "even inside an otherwise new phrase: %s.",
                 blacklist_words);

    /* Naming the exact subject text (rather than just saying "don't switch
       subjects") gives the model a concrete anchor instead of an abstract
       rule — bit-driven slot selection can still land on a candidate that
       drops the subject if the rule is vague, since selection is not
       quality-ranked. */
    char subj_clause[320] = {0};
    if (sa_len > 0)
        snprintf(subj_clause, sizeof(subj_clause),
                 "\nThe sentence's subject was established by its opening: \"%s\". "
                 "Every candidate MUST remain about that exact subject — do not "
                 "switch to a different person/thing, and do not drop the subject.",
                 subject_anchor);

    if (preamble) {
        snprintf(buf, buf_size,
            "%s"
            "Sentence so far: \"%s\"\n"
            "%s%s%s%s\n"
            "Provide %d different natural continuations with probabilities.\n"
            "Return ONLY a JSON object like: {\"goes to work\": 0.4, \"drives\": 0.3, \"takes the bus every day\": 0.2, \"commutes early\": 0.1} — probs sum to 1.0.",
            preamble, ctx ? ctx : "", phase, subj_clause, bl_clause, bw_clause, n);
    } else {
        snprintf(buf, buf_size,
            "Sentence so far: \"%s\"\n"
            "%s%s%s%s\n"
            "Provide %d different natural continuations with probabilities.\n"
            "Return ONLY a JSON object — probs sum to 1.0.",
            ctx ? ctx : "", phase, subj_clause, bl_clause, bw_clause, n);
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

static const char* FALLBACK_PHRASES[] = {
    "and the world",   "in the morning",  "at the office",  "on the street",
    "to the market",   "by the river",    "with the team",  "from the start",
    "for the cause",   "of the year",     "under the sky",  "over the hill",
    "into the night",  "through the day", "around the town", "before the dawn",
    "after the rain",  "among the trees", "beside the road", "along the way"
};
#define FALLBACK_PHRASES_COUNT 20

static LLMResponse* uniform_phrase_fallback(int n)
{
    LLMResponse* resp = (LLMResponse*)calloc(1, sizeof(LLMResponse));
    resp->candidates  = (LLMCandidate*)calloc((size_t)n, sizeof(LLMCandidate));
    resp->count       = n;
    float p = 1.0f / (float)n;
    for (int i = 0; i < n; i++) {
        strncpy(resp->candidates[i].text,
                FALLBACK_PHRASES[i % FALLBACK_PHRASES_COUNT], 63);
        resp->candidates[i].text[63] = '\0';
        resp->candidates[i].prob = p;
    }
    return resp;
}

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
    cJSON_AddBoolToObject  (req, "cache_prompt", 0);
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
    cJSON_AddNumberToObject(req, "n_predict",   96);
    cJSON_AddNumberToObject(req, "temperature", 0.0);
    cJSON_AddNumberToObject(req, "seed",        42);
    cJSON_AddBoolToObject  (req, "stream",      0);
    cJSON_AddBoolToObject  (req, "cache_prompt", 0);
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

LLMResponse* llm_client_get_phrase_dist(LLMClient*  client,
                                          const char* preamble,
                                          const char* full_context,
                                          const char* blacklist_phrases,
                                          const char* blacklist_words,
                                          const char* subject_anchor,
                                          StyleQuestion question)
{
    char* prompt = build_phrase_prompt(preamble, full_context,
                                       client->max_candidates, blacklist_phrases,
                                       blacklist_words, subject_anchor, question);
    if (!prompt) return NULL;

    /* Opening step of a sentence (subject_anchor empty) uses the fixed
       subject-first grammar — it doesn't use `question` (see
       build_phrase_prompt). Continuation steps get a per-question
       grammar that forces the connector, must be freed; the opening
       step's grammar is a string literal and must NOT be freed. Keyed
       off subject_anchor, matching build_phrase_prompt's phase
       selection — NOT full_context, which stays non-empty across
       sentence boundaries. A digression opening step is not special-cased
       here: its caller passes a temporary preamble quoting the stage-1
       answer text in place of the main preamble, so it takes the exact
       same !is_continuation path as a normal topic-anchored opening. */
    int   is_continuation = subject_anchor && subject_anchor[0];
    char* dyn_grammar      = is_continuation ? build_phrase_grammar(question) : NULL;
    if (is_continuation && !dyn_grammar) { free(prompt); return NULL; }
    const char* grammar = is_continuation ? dyn_grammar : OPENING_SUBJECT_GRAMMAR;

    cJSON* req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "prompt",      prompt);
    cJSON_AddStringToObject(req, "grammar",     grammar);
    cJSON_AddNumberToObject(req, "n_predict",   256);
    cJSON_AddNumberToObject(req, "temperature", 0.0);
    cJSON_AddNumberToObject(req, "seed",        42);
    cJSON_AddBoolToObject  (req, "stream",      0);
    cJSON_AddBoolToObject  (req, "cache_prompt", 0);
    char* body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    free(prompt);
    free(dyn_grammar);
    if (!body) return NULL;

    char* raw = HTTP_POST(client, "/completion", body);
    free(body);

    if (!raw) return uniform_phrase_fallback(client->max_candidates);

    LLMResponse* resp = parse_llm_response(raw, client->max_candidates);
    free(raw);

    if (!resp || resp->count == 0) {
        llm_response_free(resp);
        return uniform_phrase_fallback(client->max_candidates);
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
