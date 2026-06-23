#include "llm_client.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include <curl/curl.h>
#include <cJSON.h>

/* ── write callback ───────────────────────────────────────────────────────── */

typedef struct {
    char*  buf;
    size_t len;
    size_t cap;
} WriteBuffer;

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    size_t        n   = size * nmemb;
    WriteBuffer*  wb  = (WriteBuffer*)userdata;
    size_t        need = wb->len + n + 1;
    if (need > wb->cap) {
        size_t new_cap = wb->cap ? wb->cap * 2 : 4096;
        while (new_cap < need) new_cap *= 2;
        char* tmp = (char*)realloc(wb->buf, new_cap);
        if (!tmp) return 0;
        wb->buf = tmp;
        wb->cap = new_cap;
    }
    memcpy(wb->buf + wb->len, ptr, n);
    wb->len += n;
    wb->buf[wb->len] = '\0';
    return n;
}

/* ── lifecycle ────────────────────────────────────────────────────────────── */

LLMClient* llm_client_create(const char* base_url, int max_candidates, int timeout_ms)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);

    LLMClient* c = (LLMClient*)calloc(1, sizeof(LLMClient));
    if (!c) return NULL;

    strncpy(c->base_url, base_url ? base_url : "http://127.0.0.1:8080",
            sizeof(c->base_url) - 1);
    c->max_candidates = max_candidates > 0 ? max_candidates : 6;
    c->timeout_ms     = timeout_ms > 0     ? timeout_ms     : 30000;

    c->curl_handle = curl_easy_init();
    if (!c->curl_handle) { free(c); return NULL; }

    return c;
}

void llm_client_destroy(LLMClient* client)
{
    if (!client) return;
    if (client->curl_handle) curl_easy_cleanup(client->curl_handle);
    free(client);
    curl_global_cleanup();
}

/* ── GBNF grammar ─────────────────────────────────────────────────────────── */

static const char* SYLLABLE_GRAMMAR =
    "root   ::= \"{\" ws pair (ws \",\" ws pair)* ws \"}\"\n"
    "pair   ::= string ws \":\" ws number\n"
    "string ::= \"\\\"\" char+ \"\\\"\"\n"
    "char   ::= [a-z\xc2\xb7]\n"   /* allow middle-dot for EOW */
    "number ::= \"-\"? [0-9]+ (\".\" [0-9]+)?\n"
    "ws     ::= [ \\t\\n]*\n";

/* ── prompt builders ──────────────────────────────────────────────────────── */

static char* build_new_word_prompt(const char* full_context, int n)
{
    char* buf = (char*)malloc(4096);
    if (!buf) return NULL;
    snprintf(buf, 4096,
        "Text so far: \"%s\"\n"
        "You are generating the next word one syllable at a time.\n"
        "Provide the %d most natural first syllables for the next word.\n"
        "Return ONLY a JSON object with syllable strings as keys and probabilities as values.\n"
        "Probabilities must sum to 1.0. Example: {\"the\": 0.4, \"a\": 0.3, \"in\": 0.3}",
        full_context, n);
    return buf;
}

static char* build_continuation_prompt(const char* full_context,
                                        const char* partial_word, int n)
{
    char* buf = (char*)malloc(4096);
    if (!buf) return NULL;
    snprintf(buf, 4096,
        "Text so far: \"%s\"\n"
        "Word being built: \"%s\"\n"
        "Provide %d natural continuation syllables plus \"\xc2\xb7\" (end-of-word).\n"
        "Higher probability for \"\xc2\xb7\" if \"%s\" is already a natural complete word.\n"
        "Return ONLY a JSON object. Probabilities must sum to 1.0.\n"
        "Example: {\"\xc2\xb7\": 0.4, \"tion\": 0.3, \"ing\": 0.3}",
        full_context, partial_word, n - 1, partial_word);
    return buf;
}

/* ── HTTP POST ────────────────────────────────────────────────────────────── */

static char* post_completion(LLMClient* client, const char* prompt_text)
{
    CURL* curl = (CURL*)client->curl_handle;

    /* build request JSON */
    cJSON* req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "prompt",      prompt_text);
    cJSON_AddStringToObject(req, "grammar",     SYLLABLE_GRAMMAR);
    cJSON_AddNumberToObject(req, "n_predict",   128);
    cJSON_AddNumberToObject(req, "temperature", 0.0);
    cJSON_AddNumberToObject(req, "seed",        42);
    cJSON_AddBoolToObject  (req, "stream",      0);

    char* req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!req_str) return NULL;

    char url[320];
    snprintf(url, sizeof(url), "%s/completion", client->base_url);

    WriteBuffer wb = {0};
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_POST,           1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     req_str);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &wb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,     (long)client->timeout_ms);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    free(req_str);

    if (rc != CURLE_OK) {
        fprintf(stderr, "meteor: curl error: %s\n", curl_easy_strerror(rc));
        free(wb.buf);
        return NULL;
    }

    return wb.buf; /* caller frees */
}

/* ── response parsing ─────────────────────────────────────────────────────── */

/* Map transmitted EOW marker (middle-dot U+00B7 = \xc2\xb7) to internal EOW_TOKEN. */
static void map_eow(char* text)
{
    if (strcmp(text, "\xc2\xb7") == 0)
        strcpy(text, "\x01");
}

static LLMResponse* parse_llm_response(const char* raw_json, int max_candidates)
{
    /* raw_json is the llama-server response envelope; content is the grammar output */
    cJSON* env = cJSON_Parse(raw_json);
    if (!env) return NULL;

    const char* content = NULL;
    cJSON* content_item = cJSON_GetObjectItemCaseSensitive(env, "content");
    if (content_item && cJSON_IsString(content_item))
        content = content_item->valuestring;

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
    cJSON* item;
    int    i = 0;
    cJSON_ArrayForEach(item, obj) {
        if (i >= n) break;
        if (!cJSON_IsNumber(item)) { i++; continue; }
        float p = (float)item->valuedouble;
        if (p < 0.0f) p = 0.0f;
        strncpy(resp->candidates[i].text, item->string, 63);
        resp->candidates[i].text[63] = '\0';
        map_eow(resp->candidates[i].text);
        resp->candidates[i].prob = p;
        sum += p;
        i++;
        resp->count = i;
    }
    cJSON_Delete(obj);

    /* normalise */
    if (sum > 0.0f)
        for (int j = 0; j < resp->count; j++)
            resp->candidates[j].prob /= sum;

    return resp;
}

static LLMResponse* uniform_fallback(int n)
{
    LLMResponse* resp = (LLMResponse*)calloc(1, sizeof(LLMResponse));
    resp->candidates  = (LLMCandidate*)calloc((size_t)n, sizeof(LLMCandidate));
    resp->count       = n;
    float p = 1.0f / (float)n;
    for (int i = 0; i < n; i++) {
        snprintf(resp->candidates[i].text, 64, "syl%d", i);
        resp->candidates[i].prob = p;
    }
    return resp;
}

/* ── public ───────────────────────────────────────────────────────────────── */

LLMResponse* llm_client_get_syllable_dist(LLMClient*  client,
                                           const char* full_context,
                                           const char* partial_word,
                                           int         is_new_word)
{
    char* prompt = is_new_word
        ? build_new_word_prompt(full_context, client->max_candidates)
        : build_continuation_prompt(full_context, partial_word, client->max_candidates);

    if (!prompt) return NULL;

    char* raw = post_completion(client, prompt);
    free(prompt);

    if (!raw) return uniform_fallback(client->max_candidates);

    LLMResponse* resp = parse_llm_response(raw, client->max_candidates);
    free(raw);

    if (!resp || resp->count == 0) {
        llm_response_free(resp);
        return uniform_fallback(client->max_candidates);
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
    CURL* curl = (CURL*)client->curl_handle;
    char url[320];
    snprintf(url, sizeof(url), "%s/health", client->base_url);

    WriteBuffer wb = {0};
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &wb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,    3000L);

    CURLcode rc = curl_easy_perform(curl);
    free(wb.buf);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    return (rc == CURLE_OK && http_code == 200) ? 1 : 0;
}
