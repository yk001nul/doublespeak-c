# Meteor Syllabification Stegosystem — Architecture Document

> **Purpose:** Reference for Claude Code (or manual implementation) to build a portable C library
> implementing the Meteor steganographic algorithm at syllable granularity, backed by a local LLM.
> The library exposes a C ABI for reuse in C++, Python (ctypes), and C# (P/Invoke / Godot).

---

## Table of Contents

1. [Background](#1-background)
2. [System Overview](#2-system-overview)
3. [Directory Structure](#3-directory-structure)
4. [Component Specifications](#4-component-specifications)
   - 4.1 [PRNG](#41-prng)
   - 4.2 [Bit Packing](#42-bit-packing)
   - 4.3 [Meteor Core](#43-meteor-core)
   - 4.4 [Syllabifier](#44-syllabifier)
   - 4.5 [LLM Client](#45-llm-client)
   - 4.6 [Encode Pipeline](#46-encode-pipeline)
   - 4.7 [Decode Pipeline](#47-decode-pipeline)
5. [Public C API](#5-public-c-api)
6. [Determinism Requirements](#6-determinism-requirements)
7. [FFI Bindings](#7-ffi-bindings)
8. [Dependencies](#8-dependencies)
9. [Build System](#9-build-system)
   - 9.1 [CMakeLists.txt](#91-cmakeliststxt)
   - 9.2 [Startup script template](#92-startup-script-template)
   - 9.3 [Build commands](#93-build-commands)
10. [Error Handling](#10-error-handling)
11. [Testing Strategy](#11-testing-strategy)
12. [Known Limitations & Future Work](#12-known-limitations--future-work)

---

## 1. Background

### What is Meteor?

Meteor (Kaptchuk et al., CCS 2021) is a provably secure steganographic system. It encodes a
secret message into a cover text by replacing the language model's usual random number
generator with pseudorandom bits derived from the message and a shared key. The resulting
text is computationally indistinguishable from normal model output to any party that does not
hold the key.

**Core encode step (one token):**

```
mask  = PRG(key)              -- β pseudorandom bits
r     = message_bits XOR mask -- β-bit index into distribution
token = lookup(distribution, r) -- token whose slot contains r
```

**Core decode step (one token):**

```
mask           = PRG(key)     -- same PRG state, same key
common_prefix  = bits shared by all r values that map to this token
message_bits   = common_prefix XOR mask[:len(common_prefix)]
```

The number of bits recovered per token equals the length of the common prefix of the token's
slot range, which is determined by the token's probability mass in the distribution. High-
probability tokens (wide slots) yield short common prefixes and encode few bits. Low-
probability tokens (narrow slots) encode more bits. This is the information-theoretic optimal
behaviour — it exactly mirrors Huffman coding.

### Why syllable-level?

English words average ~1.8 syllables. Working at syllable granularity gives ~1.8–3× more
Meteor steps per word compared to word-level selection. For a word like "in·ves·ti·ga·tion"
(5 syllables + 1 end-of-word token = 6 steps) vs 1 word-level step, the capacity gain is 6×
for that token, less on average but still substantial.

A special end-of-word marker token `EOW` (suggested byte value `0x00` internally, never
transmitted) is included in the distribution at every continuation step. Its probability
reflects how naturally the partial word reads as a complete word.

### Security model

Security reduces to the pseudorandomness of the PRG: an observer without the key sees
output drawn from the same distribution as normal model generation, because `r` is uniformly
distributed from their perspective (PRG output is indistinguishable from random). The
syllabifier must be deterministic and shared — it is the only component that must be
identical on both sides without relying on the LLM.

The PRG must be a **cryptographically secure PRNG (CSPRNG)**. A non-cryptographic PRNG
(such as xorshift32 with a 32-bit state) limits the effective key space to 2³² regardless
of key length, making the system trivially brute-forceable. This library uses ChaCha20
as the CSPRNG with a 256-bit key, providing 128-bit security against exhaustive search.
Keys are derived from caller-supplied material via HKDF-SHA256, so any input — a passphrase,
an ECDH shared secret, or an RSA-derived secret — is safely reduced to the 256-bit key
ChaCha20 requires. Both libsodium functions are used via the `libsodium` dependency.

---

## 2. System Overview

```
  ENCODER                                    DECODER
  ───────                                    ───────

  message + key                              covertext + key
       │                                          │
       ▼                                          ▼
  [Bit Packer]                             [Syllabifier]
  message → bit array                      covertext → syllable sequence
       │                                          │
       ▼                                          ▼
  [PRNG] ← key                             [PRNG] ← key (same seed)
  generates mask bits                      generates mask bits
       │                                          │
       ▼                                          ▼
  [LLM Client]                             [LLM Client]
  syllable distribution                    same distribution (deterministic)
  at each step                             at each step
       │                                          │
       ▼                                          ▼
  [Meteor Step]                            [Meteor Step]
  r = msg_bits XOR mask                    common_prefix XOR mask
  → pick syllable                          → recover bits
       │                                          │
       ▼                                          ▼
  covertext (text)                         message (bytes)
```

Both sides must use **the same GGUF model file** and **the same inference settings** to
guarantee identical probability distributions. See §6 for determinism requirements.

---

## 3. Directory Structure

```
meteor_stego/
├── CMakeLists.txt
├── README.md
├── include/
│   └── meteor.h                  # Public API header (C ABI)
├── src/
│   ├── meteor.c                  # Library entry points, context management
│   ├── prng.c / prng.h           # ChaCha20 CSPRNG + HKDF key derivation
│   ├── bits.c / bits.h           # Bit packing / unpacking
│   ├── meteor_core.c / .h        # Meteor step (encode + decode)
│   ├── syllabifier.c / .h        # libhyphen wrapper + fallback heuristic
│   ├── llm_client.c / .h         # HTTP client (libcurl) + JSON (cJSON)
│   ├── encode.c / .h             # Full encode pipeline
│   └── decode.c / .h             # Full decode pipeline
├── third_party/
│   ├── cjson/                    # cJSON (single .c/.h, MIT)
│   └── libhyphen/                # libhyphen headers (link system lib or vendor)
├── bindings/
│   ├── python/
│   │   └── meteor.py             # ctypes wrapper
│   └── csharp/
│       └── Meteor.cs             # P/Invoke wrapper (Godot-compatible)
├── tests/
│   ├── test_prng.c
│   ├── test_bits.c
│   ├── test_meteor_core.c
│   ├── test_syllabifier.c
│   ├── test_roundtrip.c          # encode → decode, verify message recovery
│   └── test_determinism.c        # two encode runs same key → same covertext
├── scripts/
│   ├── start_llama_server.sh     # launch llama-server with deterministic settings (Linux/macOS)
│   ├── start_llama_server.bat    # Windows equivalent
│   └── verify_model.sh           # SHA-256 check model file against expected hash
└── data/
    └── hyph_en_US.dic            # libhyphen dictionary (Apache-licensed)
```

---

## 4. Component Specifications

### 4.1 PRNG

**File:** `src/prng.c` / `src/prng.h`

**Algorithm:** ChaCha20 stream cipher used as a CSPRNG, keyed with a 256-bit key derived
via HKDF-SHA256. Both primitives are provided by libsodium.

**Why not xorshift32:** A 32-bit internal state limits the effective key space to 2³²
regardless of how large the caller's key is. An attacker can exhaust that space in seconds.
ChaCha20 with a 256-bit key provides 2²⁵⁶ possible streams and is computationally
indistinguishable from random — the security property Meteor's proof requires of the PRG.

**Why not RSA key sizes directly:** RSA key sizes measure modulus length, not security
level. RSA-2048 provides ~112-bit security equivalent; RSA-3072 provides ~128-bit. These
are asymmetric primitives used for key exchange, not stream generation. The correct pattern
is to use RSA/ECDH to establish a shared secret, then pass that secret through HKDF to
derive the 256-bit ChaCha20 key used here.

**Key derivation pipeline:**

```
caller input (passphrase, ECDH output, RSA shared secret, raw bytes, …)
                        │
                        ▼
              HKDF-SHA256 (libsodium)
              salt  = random 32-byte value, shared out-of-band
              info  = "meteor-stego-v1" (application context label)
                        │
                        ▼
                256-bit symmetric key
                        │
                        ▼
              ChaCha20 keystream (CSPRNG)
```

**Structs and API:**

```c
#include <sodium.h>

// PRNG_BLOCK_SIZE is ChaCha20's native block size
#define PRNG_BLOCK_BYTES 64

typedef struct {
    uint8_t  key[32];              // 256-bit ChaCha20 key (derived via HKDF)
    uint8_t  nonce[8];             // 64-bit nonce; zeroed for deterministic use
    uint64_t block_counter;        // advances every PRNG_BLOCK_BYTES consumed
    uint8_t  block[PRNG_BLOCK_BYTES]; // current keystream block
    int      block_pos;            // byte position within current block (0–63)
    int      bit_pos;              // bit position within current byte (0–7)
} MeteorPRNG;

// Derive key from arbitrary input material and initialise PRNG.
// input / input_len : any caller-supplied key material (passphrase, shared secret, …)
// salt / salt_len   : random 32-byte value shared between encoder and decoder
//                     out-of-band; may be NULL for testing (uses zero salt).
// Returns 0 on success, -1 on libsodium error.
int  prng_init(MeteorPRNG* prng,
               const uint8_t* input, size_t input_len,
               const uint8_t* salt,  size_t salt_len);

// Return next n bits (1 ≤ n ≤ 31) as uint32_t, MSB-first.
// Advances internal state; never fails once initialised.
uint32_t prng_next_bits(MeteorPRNG* prng, int n);

// Securely wipe key material from memory when context is destroyed.
void prng_wipe(MeteorPRNG* prng);
```

**Key derivation (HKDF-SHA256):**

```c
int prng_init(MeteorPRNG* prng,
              const uint8_t* input, size_t input_len,
              const uint8_t* salt,  size_t salt_len) {

    if (sodium_init() < 0) return -1;  // idempotent; safe to call repeatedly

    static const uint8_t APP_INFO[] = "meteor-stego-v1";

    // HKDF: extract PRK from input material + salt, then expand to 32-byte key
    uint8_t prk[crypto_auth_hmacsha256_BYTES];
    const uint8_t zero_salt[32] = {0};
    const uint8_t* effective_salt = (salt && salt_len > 0) ? salt : zero_salt;
    size_t effective_salt_len     = (salt && salt_len > 0) ? salt_len : sizeof(zero_salt);

    // Extract
    crypto_auth_hmacsha256_state st;
    crypto_auth_hmacsha256_init(&st, effective_salt, effective_salt_len);
    crypto_auth_hmacsha256_update(&st, input, input_len);
    crypto_auth_hmacsha256_final(&st, prk);

    // Expand (one round: OKM = HMAC(PRK, info || 0x01))
    crypto_auth_hmacsha256_init(&st, prk, sizeof(prk));
    crypto_auth_hmacsha256_update(&st, APP_INFO, sizeof(APP_INFO) - 1);
    uint8_t counter = 0x01;
    crypto_auth_hmacsha256_update(&st, &counter, 1);
    crypto_auth_hmacsha256_final(&st, prng->key);  // writes 32 bytes into key

    // Initialise nonce and counters
    memset(prng->nonce, 0, sizeof(prng->nonce));
    prng->block_counter = 0;
    prng->block_pos     = PRNG_BLOCK_BYTES;   // force refill on first call
    prng->bit_pos       = 0;

    // Wipe intermediate PRK from stack
    sodium_memzero(prk, sizeof(prk));
    return 0;
}
```

**Bit extraction (ChaCha20 keystream):**

```c
uint32_t prng_next_bits(MeteorPRNG* prng, int n) {
    uint32_t result = 0;
    for (int i = 0; i < n; i++) {
        // Refill block when exhausted
        if (prng->block_pos >= PRNG_BLOCK_BYTES) {
            // XOR a zero buffer with the keystream = pure keystream output
            static const uint8_t zeros[PRNG_BLOCK_BYTES] = {0};
            crypto_stream_chacha20_xor_ic(
                prng->block, zeros, PRNG_BLOCK_BYTES,
                prng->nonce, prng->block_counter++, prng->key);
            prng->block_pos = 0;
            prng->bit_pos   = 0;
        }
        // Extract one bit MSB-first from the current byte
        uint8_t byte = prng->block[prng->block_pos];
        result = (result << 1) | ((byte >> (7 - prng->bit_pos)) & 1);
        prng->bit_pos++;
        if (prng->bit_pos >= 8) {
            prng->bit_pos = 0;
            prng->block_pos++;
        }
    }
    return result;
}
```

**Secure wipe on destroy:**

```c
void prng_wipe(MeteorPRNG* prng) {
    sodium_memzero(prng, sizeof(MeteorPRNG));
}
```

> **Note:** `sodium_memzero` is guaranteed not to be optimised away by the compiler,
> unlike plain `memset`. Call `prng_wipe` inside `meteor_destroy` before freeing the
> context to ensure the 256-bit key does not linger in heap memory.

> **State ordering:** The PRNG state advances one call to `prng_next_bits(β)` per syllable
> token, including EOW tokens. Encoder and decoder must advance state in identical order.
> Any divergence in token sequence (e.g. a syllabification disagreement) will cause the
> PRNG state to drift and corrupt all subsequent bit recovery.

---

### 4.2 Bit Packing

**File:** `src/bits.c` / `src/bits.h`

```c
// Convert message bytes to bit array (MSB first per byte)
// Appends 8 zero bits as null terminator
// Caller frees returned buffer
uint8_t* bits_from_bytes(const uint8_t* msg, size_t msg_len, size_t* out_bit_count);

// Convert bit array back to bytes, stopping at 8 consecutive zero bits
// Caller frees returned buffer
uint8_t* bits_to_bytes(const uint8_t* bits, size_t bit_count, size_t* out_msg_len);

// Extract n bits starting at offset from a bit array
uint32_t bits_read(const uint8_t* bits, size_t offset, int n);
```

**Bit order:** big-endian per byte — bit 7 of byte 0 is bits[0], bit 0 of byte 0 is bits[7].

**Null terminator:** 8 zero bits appended so the decoder knows when the message ends.
Messages that contain embedded null bytes must be length-prefixed by the caller before
passing to `meteor_encode()`.

---

### 4.3 Meteor Core

**File:** `src/meteor_core.c` / `src/meteor_core.h`

```c
// One entry in a syllable distribution
typedef struct {
    char    text[64];   // syllable string, or EOW_TOKEN for end-of-word
    float   p;          // probability (0.0 < p ≤ 1.0, sum across dist = 1.0)
    int     slot_start; // inclusive, in [0, 2^beta)
    int     slot_end;   // inclusive
    int     slot_count; // slot_end - slot_start + 1
} MeteorSlot;

typedef struct {
    MeteorSlot* slots;
    int         count;
    int         beta;
    int         total_slots; // 2^beta
} MeteorDist;

// Result of one encode step
typedef struct {
    char     chosen[64];    // syllable chosen (or EOW_TOKEN)
    uint32_t mask_bits;
    uint32_t r;
    char     mask_bin[33];  // null-terminated binary string, for debugging
    char     r_bin[33];
    int      cp_len;        // common prefix length (bits recovered)
    uint8_t  recovered[32]; // recovered bits, length = cp_len
} MeteorStepResult;

#define EOW_TOKEN "\x01"  // internal marker for end-of-word; never written to output

// Build slot table from a probability distribution
// Normalises probabilities and assigns integer slot ranges in [0, 2^beta)
MeteorDist* meteor_build_dist(const char** syllables, const float* probs,
                               int count, int beta);
void        meteor_free_dist(MeteorDist* dist);

// Encode one step: consumes up to beta bits from msg_bits[bit_offset..]
// Advances prng state by beta bits
// Returns step result; caller does not free (stack-allocated return)
MeteorStepResult meteor_encode_step(const MeteorDist* dist,
                                    const uint8_t* msg_bits, size_t bit_offset,
                                    size_t total_bits,
                                    MeteorPRNG* prng, int beta);

// Decode one step: given the chosen syllable and distribution, recover bits
// Advances prng state by beta bits
// Returns number of bits recovered; writes to out_bits (caller provides buffer ≥ beta)
int meteor_decode_step(const char* chosen_syllable,
                       const MeteorDist* dist,
                       MeteorPRNG* prng, int beta,
                       uint8_t* out_bits);
```

**Slot building algorithm:**

```c
// Assign each syllable a contiguous range of integers in [0, 2^beta)
// proportional to its probability, using rounding with remainder correction.
for (int i = 0; i < count; i++) {
    int count_i = (int)roundf(probs[i] * total_slots);
    count_i = max(1, count_i);            // every candidate gets at least 1 slot
    slots[i].slot_start = cursor;
    slots[i].slot_end   = cursor + count_i - 1;
    slots[i].slot_count = count_i;
    cursor += count_i;
}
// Clamp last slot to exactly total_slots - 1 to absorb rounding errors
slots[count-1].slot_end   = total_slots - 1;
slots[count-1].slot_count = total_slots - slots[count-1].slot_start;
```

**Common prefix length:**

```c
// The receiver can only recover bits whose value is identical across
// every r that would map to this token's slot range.
// Binary representations of slot_start and slot_end share a common prefix
// of length cp_len. Only those bits are recoverable.
int common_prefix_len(int slot_start, int slot_end, int beta) {
    if (slot_start == slot_end) return beta; // single slot: all bits known
    int cp = 0;
    for (int i = beta - 1; i >= 0; i--) {
        if (((slot_start >> i) & 1) == ((slot_end >> i) & 1)) cp++;
        else break;
    }
    return cp;
}
```

---

### 4.4 Syllabifier

**File:** `src/syllabifier.c` / `src/syllabifier.h`

```c
// Split word into syllables.
// out_syllables: array of pointers to null-terminated strings.
// Returns number of syllables. Caller frees out_syllables and each string.
int syllabify_word(const char* word, char*** out_syllables);

// Syllabify all words in a text.
// Returns a flat sequence of (word_index, syllable_index, syllable_string) triples.
// Used by the decoder to iterate through the received covertext.
typedef struct {
    int  word_index;
    int  syl_index;
    int  syl_count;       // total syllables in this word
    char syllable[64];
    int  is_last_in_word; // 1 if this is the final syllable (EOW follows)
} SylToken;

SylToken* syllabify_text(const char* text, int* out_count);
void      syllabify_text_free(SylToken* tokens);
```

**Primary implementation — libhyphen:**

```c
#include <hyphen.h>

// Load dictionary once at startup; store in MeteorCtx
HyphenDict* dict = hnj_hyphen_load("data/hyph_en_US.dic");

int syllabify_word_libhyphen(HyphenDict* dict, const char* word,
                              char*** out_syllables) {
    char*  hyphenated = NULL;
    char** rep        = NULL;
    int*   pos        = NULL;
    int*   cut        = NULL;

    // hnj_hyphen_hyphenate2 writes '-' at each valid break point
    hnj_hyphen_hyphenate2(dict, word, strlen(word),
                          &hyphenated, NULL, &rep, &pos, &cut);

    // Split hyphenated string on '-' to get syllables
    // ... (standard string splitting)

    free(hyphenated);
    // free rep, pos, cut if non-null
    return syl_count;
}
```

**Fallback — custom Knuth-Liang heuristic:**

> ⚠ Use this if libhyphen is unavailable or fails to load the dictionary.
> Based on the vowel-cluster + VC|CV rules derived from hyphen.tex patterns.
> Accuracy ~80–85% on common English; sufficient for development and testing
> but may cause occasional decode bit errors on polysyllabic or irregular words.

```c
// Tier 1 fallback (no external dependency)
int syllabify_word_heuristic(const char* word, char*** out_syllables) {
    // 1. Strip non-alpha chars, lowercase
    // 2. Locate vowel nuclei (contiguous runs of [aeiouy])
    // 3. Apply rules per nucleus-to-nucleus gap:
    //    - 0 consonants between nuclei: check known diphthongs; break if not
    //    - 1 consonant:  V|CV  (consonant goes with next syllable)
    //    - 2 consonants: VC|CV (split between them)
    //    - 3+ consonants: keep first group with preceding vowel, rest with next
    // 4. Keep known digraphs (ch, sh, th, wh, ph, ck, ng, qu, tr, br, ...)
    //    intact — never break across a digraph
    // 5. Silent-e rule: trailing 'e' after consonant does not form a syllable
    //    unless it follows a vowel (e.g. "ee", "ae")
    // 6. Common suffix protection: -tion, -sion, -ture, -age → kept together
}
```

> **Reminder (personal note):** You have a custom C implementation derived directly from
> the `hyphen.tex` pattern table. Use that as a drop-in replacement for the heuristic if
> libhyphen's dictionary is unavailable — it should match TeX hyphenation accuracy (~99.6%)
> without the runtime dependency.

---

### 4.5 LLM Client

**File:** `src/llm_client.c` / `src/llm_client.h`

**Target backend:** llama.cpp server (`llama-server`) running locally.
Start the server with:

```bash
./llama-server \
  --model path/to/model.gguf \
  --port 8080 \
  --n-predict 256 \
  --temp 0.0 \
  --seed 42 \
  --threads 1 \
  --no-mmap         # avoids OS-level non-determinism on some platforms
```

**Key server parameters for determinism:**

| Parameter | Value | Reason |
|---|---|---|
| `--temp` | `0.0` | Greedy logit ordering; note: we read logprobs, not sampled tokens |
| `--seed` | fixed int | Reproducible sampling internals |
| `--threads` | `1` | Eliminates floating-point reordering from multithreaded reduction |
| `--no-mmap` | — | Avoids memory-mapped file caching differences across platforms |
| `--ctx-size` | ≥ 2048 | Enough context for typical covertext |

**API endpoints used:**

- `POST /completion` with grammar-constrained JSON output (see below)
- `GET /health` for startup check

**LLM client structs:**

```c
typedef struct {
    char  text[64];
    float prob;
} LLMCandidate;

typedef struct {
    LLMCandidate* candidates;
    int           count;
} LLMResponse;

typedef struct {
    char    base_url[256];    // e.g. "http://127.0.0.1:8080"
    int     timeout_ms;
    int     max_candidates;   // typically 5–8
    void*   curl_handle;      // CURL* (opaque to callers)
} LLMClient;

LLMClient*   llm_client_create(const char* base_url, int max_candidates);
void         llm_client_destroy(LLMClient* client);

// Get syllable distribution for one Meteor step
// is_new_word: 1 = first syllable of a new word, 0 = continuation/EOW
// partial_word: syllables built so far for current word (empty string if is_new_word)
// full_context: entire generated text so far
LLMResponse* llm_client_get_syllable_dist(LLMClient* client,
                                           const char* full_context,
                                           const char* partial_word,
                                           int is_new_word);
void         llm_response_free(LLMResponse* resp);
```

**Grammar-constrained output (GBNF):**

Use llama.cpp's grammar feature to force the model to output valid JSON without markdown
fences or extra text. This eliminates all JSON parsing errors.

```c
// GBNF grammar for {"syl": prob, ...} with exactly N entries
// Build dynamically based on max_candidates
static const char* SYLLABLE_GRAMMAR =
    "root   ::= \"{\" pair (\",\" pair)* \"}\"\n"
    "pair   ::= string \":\" number\n"
    "string ::= \"\\\"\" [a-z\xc2\xb7]+ \"\\\"\"\n"  // allow middle-dot for EOW
    "number ::= [0-9] \".\" [0-9]+\n";
```

**Prompts:**

New-word prompt (first syllable):

```c
// Build dynamically:
// "Text so far: \"{full_context}\"\n"
// "You are generating the next word one syllable at a time.\n"
// "Provide the {N} most natural first syllables for the next word.\n"
// "Return ONLY a JSON object: {\"syl1\": prob, ...} — probs sum to 1.0."
```

Continuation prompt:

```c
// "Text so far: \"{full_context}\"\n"
// "Word being built: \"{partial_word}\"\n"
// "Provide {N-1} natural continuation syllables plus \"\\u00b7\" (end-of-word).\n"
// "Higher prob for \"\\u00b7\" if \"{partial_word}\" is already a natural word.\n"
// "Return ONLY a JSON object: {\"\\u00b7\": prob, ...} — probs sum to 1.0."
```

> `\u00b7` (middle dot `·`) is the transmitted EOW marker. Internally mapped to `EOW_TOKEN`
> (`\x01`) in C strings to avoid confusion with any natural text character.

**Response parsing:**

```c
LLMResponse* llm_parse_response(const char* json_str) {
    cJSON* root = cJSON_Parse(json_str);
    // Iterate over object keys; each key is a syllable, value is a probability
    // Normalise probabilities to sum to 1.0
    // Map "·" to EOW_TOKEN internally
    // Sort descending by probability
    // Return LLMResponse*
}
```

**HTTP request (libcurl):**

```c
// Use a reusable CURL handle stored in LLMClient (avoids TCP reconnect per call)
// Set Content-Type: application/json
// POST body: JSON with "prompt", "grammar", "n_predict", "temperature", "seed"
// Response: accumulate in a write callback into a dynamically grown buffer
// Parse JSON response field "content" for the grammar-constrained output
```

---

### 4.6 Encode Pipeline

**File:** `src/encode.c` / `src/encode.h`

```c
char* meteor_encode_impl(MeteorCtx* ctx,
                         const uint8_t* message, size_t msg_len,
                         const char* starting_context,
                         int* out_error);
```

**Algorithm:**

```
1.  Convert message to bit array (bits_from_bytes), append null terminator
2.  Seed PRNG from key
3.  full_text = starting_context
    partial_word_syls = []   (empty)
    bit_offset = 0

4.  LOOP while bit_offset < total_bits AND steps < MAX_STEPS:

    a.  is_new_word = (len(partial_word_syls) == 0)

    b.  Call LLM: dist = llm_client_get_syllable_dist(
                            full_text, join(partial_word_syls), is_new_word)

    c.  Build MeteorDist from dist (meteor_build_dist)

    d.  step = meteor_encode_step(dist, msg_bits, bit_offset, total_bits, prng, beta)

    e.  bit_offset += step.cp_len

    f.  IF step.chosen == EOW_TOKEN:
            word = join(partial_word_syls)      -- concatenate syllables
            full_text = full_text + " " + word
            append word to output token list
            partial_word_syls = []
        ELSE:
            partial_word_syls.append(step.chosen)

5.  IF partial_word_syls non-empty (message ended mid-word):
        -- EOW was not chosen; finalise word anyway, no more bits embedded
        word = join(partial_word_syls)
        full_text = full_text + " " + word

6.  Return full_text as null-terminated string (heap-allocated)
```

**MAX_STEPS:** 256 by default (configurable in MeteorCtx). Acts as safety cap to prevent
infinite loops if the message is too long for the LLM to finish in reasonable context.

**Memory:** `full_text` is grown with `realloc` as words are appended. Initial allocation:
`strlen(starting_context) + MAX_STEPS * 32` bytes.

---

### 4.7 Decode Pipeline

**File:** `src/decode.c` / `src/decode.h`

```c
uint8_t* meteor_decode_impl(MeteorCtx* ctx,
                            const char* covertext,
                            size_t* out_msg_len,
                            int* out_error);
```

**Algorithm:**

```
1.  Seed PRNG from key (same seed as encoder)
2.  Syllabify covertext: tokens = syllabify_text(covertext)
    -- tokens = [(word_idx, syl_idx, syllable, is_last_in_word), ...]
    -- Skip starting_context prefix words (by word count)

3.  recovered_bits = []
    partial_word_syls = []
    full_text_reconstructed = starting_context

4.  FOR each syl_token in tokens:

    a.  is_new_word = (syl_token.syl_index == 0)

    b.  Call LLM (same prompt as encoder):
            dist = llm_client_get_syllable_dist(
                       full_text_reconstructed,
                       join(partial_word_syls),
                       is_new_word)

    c.  Build MeteorDist

    d.  bits = meteor_decode_step(syl_token.syllable, dist, prng, beta, buf)
        recovered_bits.append(bits[:cp_len])

    e.  IF syl_token.is_last_in_word:
            -- Synthesise the EOW step (it is always present in the encode sequence)
            eow_dist = llm_client_get_syllable_dist(
                           full_text_reconstructed,
                           syl_token.syllable,  -- partial word = completed word
                           0)
            eow_bits = meteor_decode_step(EOW_TOKEN, eow_dist, prng, beta, buf)
            recovered_bits.append(eow_bits[:cp_len])

            word = join(partial_word_syls) + syl_token.syllable
            full_text_reconstructed += " " + word
            partial_word_syls = []
        ELSE:
            partial_word_syls.append(syl_token.syllable)

    f.  IF null_terminator_found(recovered_bits):
            BREAK

5.  Return bits_to_bytes(recovered_bits)
```

**Null terminator detection:** After each byte boundary in `recovered_bits`, check if the
last 8 bits are all zero. If so, stop and return everything before those 8 bits.

> ⚠ **Critical:** The decoder must reconstruct `full_text_reconstructed` in exactly the same
> order and format as the encoder built `full_text`. This includes the same space separator,
> same word boundaries, and same starting context prefix. Any divergence produces different
> LLM prompts → different distributions → wrong bits recovered.

---

## 5. Public C API

**File:** `include/meteor.h`

```c
#ifndef METEOR_H
#define METEOR_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Opaque context ────────────────────────────────────────────────────────────

typedef struct MeteorCtx MeteorCtx;

// ── Configuration ─────────────────────────────────────────────────────────────

typedef struct {
    // Key material — provide ONE of the following two options:
    //
    // Option A: raw 256-bit key (already derived by caller, e.g. from ECDH)
    //   Set key_raw to point to 32 bytes; set key_input / key_input_len to NULL/0.
    //
    // Option B: arbitrary input material (passphrase, RSA shared secret, …)
    //   Set key_input / key_input_len; set key_raw to NULL.
    //   The library derives a 256-bit key via HKDF-SHA256 internally.
    //
    const uint8_t* key_raw;          // 32 bytes, or NULL
    const uint8_t* key_input;        // arbitrary bytes, or NULL
    size_t         key_input_len;
    const uint8_t* salt;             // 32-byte HKDF salt (shared out-of-band); NULL = zero salt
    size_t         salt_len;

    int         beta;            // bits per Meteor step (2–5 recommended; default 3)
    int         num_candidates;  // syllable candidates per LLM call (4–8; default 6)
    const char* llm_url;         // local server URL, e.g. "http://127.0.0.1:8080"
    const char* hyphen_dict;     // path to hyph_en_US.dic (NULL = use heuristic)
    int         max_steps;       // max syllable steps before giving up (default 256)
    int         llm_timeout_ms;  // HTTP timeout per LLM call (default 30000)
} MeteorConfig;

// ── Lifecycle ─────────────────────────────────────────────────────────────────

MeteorCtx* meteor_create(const MeteorConfig* config);
void       meteor_destroy(MeteorCtx* ctx);

// ── Encode ────────────────────────────────────────────────────────────────────

// Encodes message into covertext.
// starting_context: plain text that both sender and receiver already share
//                   (e.g. "Researchers announced"). Acts as LLM prompt seed.
// Returns heap-allocated null-terminated covertext string on success, NULL on error.
// Caller must call meteor_free() on the returned pointer.
char* meteor_encode(MeteorCtx*     ctx,
                    const uint8_t* message,
                    size_t         msg_len,
                    const char*    starting_context,
                    int*           out_error);

// ── Decode ────────────────────────────────────────────────────────────────────

// Decodes covertext back to message bytes.
// starting_context: must be identical to the one used during encode.
// Returns heap-allocated message bytes on success, NULL on error.
// Caller must call meteor_free() on the returned pointer.
// out_msg_len: set to the number of recovered bytes.
uint8_t* meteor_decode(MeteorCtx*  ctx,
                       const char* covertext,
                       const char* starting_context,
                       size_t*     out_msg_len,
                       int*        out_error);

// ── Utilities ─────────────────────────────────────────────────────────────────

// Syllabify a single word using libhyphen (or heuristic fallback).
// Returns null-terminated string with syllables joined by middle-dot U+00B7
// e.g. "re·mark·a·ble". Caller must call meteor_free().
char* meteor_syllabify_word(MeteorCtx* ctx, const char* word);

// Check LLM server health. Returns 1 if reachable, 0 otherwise.
int meteor_llm_health(MeteorCtx* ctx);

// Free any pointer returned by this library.
void meteor_free(void* ptr);

// ── Error codes ───────────────────────────────────────────────────────────────

#define METEOR_OK              0
#define METEOR_ERR_CONFIG      1   // invalid configuration
#define METEOR_ERR_LLM         2   // LLM server unreachable or returned bad response
#define METEOR_ERR_CAPACITY    3   // message too long for max_steps
#define METEOR_ERR_DECODE      4   // syllabification or distribution mismatch
#define METEOR_ERR_DICT        5   // hyphenation dictionary not found
#define METEOR_ERR_OOM         6   // memory allocation failed
#define METEOR_ERR_TIMEOUT     7   // LLM call timed out
#define METEOR_ERR_CRYPTO      8   // libsodium initialisation or HKDF failure

#ifdef __cplusplus
}
#endif

#endif /* METEOR_H */
```

---

## 6. Determinism Requirements

This is the most critical operational requirement. A single-bit difference in the
probability distribution between encoder and decoder causes all subsequent bits to
be wrong.

**Checklist — both sides must match:**

| Item | Requirement |
|---|---|
| Model file | Identical GGUF file (verify with SHA-256) |
| Quantisation | Same quantisation level (e.g. both Q4_K_M or both F16) |
| Inference threads | `n_threads = 1` (eliminates reduction order non-determinism) |
| Temperature | `0.0` (note: we read logprobs not sampled tokens; still set this) |
| Seed | Same fixed integer (e.g. `42`) |
| Context format | Byte-identical prompt strings (same starting_context, same separators) |
| Memory mapping | Disabled (`--no-mmap`) on platforms where it causes variance |
| Hardware | CPU-only recommended; GPU float rounding differs across vendors |
| llama.cpp version | Pin to a specific commit/tag; floating-point behaviour can change between versions |

**Verification procedure:**

Write a test (`tests/test_determinism.c`) that:
1. Calls `llm_client_get_syllable_dist` twice with the same arguments
2. Asserts that all probabilities match to at least 4 decimal places
3. Run on both encoder and decoder machines before deployment

If probabilities diverge, do not proceed — message recovery will fail silently
(no error, wrong bits).

**Tolerance:** A small float rounding difference (≤ 1e-5) in a probability that does not
change slot boundaries is acceptable. A difference that shifts a slot boundary by ≥ 1
position will flip bits. The test should therefore compare slot assignments, not raw floats.

---

## 7. FFI Bindings

### Python (ctypes)

**File:** `bindings/python/meteor.py`

```python
import ctypes, os

_lib = ctypes.CDLL(os.path.join(os.path.dirname(__file__), "libmeteor.so"))

class MeteorConfig(ctypes.Structure):
    _fields_ = [
        ("key_raw",        ctypes.c_char_p),   # 32 bytes or None
        ("key_input",      ctypes.c_char_p),   # arbitrary bytes or None
        ("key_input_len",  ctypes.c_size_t),
        ("salt",           ctypes.c_char_p),   # 32-byte HKDF salt or None
        ("salt_len",       ctypes.c_size_t),
        ("beta",           ctypes.c_int),
        ("num_candidates", ctypes.c_int),
        ("llm_url",        ctypes.c_char_p),
        ("hyphen_dict",    ctypes.c_char_p),
        ("max_steps",      ctypes.c_int),
        ("llm_timeout_ms", ctypes.c_int),
    ]

_lib.meteor_create.restype  = ctypes.c_void_p
_lib.meteor_create.argtypes = [ctypes.POINTER(MeteorConfig)]
_lib.meteor_encode.restype  = ctypes.c_char_p
_lib.meteor_encode.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                ctypes.c_size_t, ctypes.c_char_p,
                                ctypes.POINTER(ctypes.c_int)]
_lib.meteor_decode.restype  = ctypes.POINTER(ctypes.c_uint8)
_lib.meteor_decode.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
                                ctypes.POINTER(ctypes.c_size_t),
                                ctypes.POINTER(ctypes.c_int)]
_lib.meteor_free.restype    = None
_lib.meteor_free.argtypes   = [ctypes.c_void_p]
_lib.meteor_destroy.restype = None
_lib.meteor_destroy.argtypes= [ctypes.c_void_p]

class Meteor:
    def __init__(self, key_input: bytes, salt: bytes = None,
                 beta: int = 3, num_candidates: int = 6,
                 llm_url: str = "http://127.0.0.1:8080",
                 hyphen_dict: str = "data/hyph_en_US.dic"):
        cfg = MeteorConfig(
            key_raw        = None,
            key_input      = key_input,
            key_input_len  = len(key_input),
            salt           = salt,
            salt_len       = len(salt) if salt else 0,
            beta           = beta,
            num_candidates = num_candidates,
            llm_url        = llm_url.encode(),
            hyphen_dict    = hyphen_dict.encode() if hyphen_dict else None,
            max_steps      = 256,
            llm_timeout_ms = 30000,
        )
        self._ctx = _lib.meteor_create(ctypes.byref(cfg))

    def encode(self, message: bytes, starting_context: str) -> str:
        err = ctypes.c_int(0)
        result = _lib.meteor_encode(self._ctx, message, len(message),
                                    starting_context.encode(), ctypes.byref(err))
        if err.value != 0:
            raise RuntimeError(f"meteor_encode error {err.value}")
        text = result.decode("utf-8")
        _lib.meteor_free(result)
        return text

    def decode(self, covertext: str, starting_context: str) -> bytes:
        err     = ctypes.c_int(0)
        msg_len = ctypes.c_size_t(0)
        result  = _lib.meteor_decode(self._ctx, covertext.encode(),
                                     starting_context.encode(),
                                     ctypes.byref(msg_len), ctypes.byref(err))
        if err.value != 0:
            raise RuntimeError(f"meteor_decode error {err.value}")
        out = bytes(result[:msg_len.value])
        _lib.meteor_free(result)
        return out

    def __del__(self):
        if self._ctx:
            _lib.meteor_destroy(self._ctx)
```

### C# / Godot P/Invoke

**File:** `bindings/csharp/Meteor.cs`

```csharp
using System;
using System.Runtime.InteropServices;

namespace MeteorStego
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct MeteorConfig
    {
        public IntPtr  KeyRaw;          // 32-byte raw key pointer, or IntPtr.Zero
        public IntPtr  KeyInput;        // arbitrary key material pointer, or IntPtr.Zero
        public UIntPtr KeyInputLen;
        public IntPtr  Salt;            // 32-byte HKDF salt pointer, or IntPtr.Zero
        public UIntPtr SaltLen;
        public int     Beta;
        public int     NumCandidates;
        public string  LlmUrl;
        public string  HyphenDict;
        public int     MaxSteps;
        public int     LlmTimeoutMs;
    }

    public static class MeteorNative
    {
        // Library name resolved by .NET runtime:
        //   Windows → meteor.dll
        //   Linux   → libmeteor.so
        //   macOS   → libmeteor.dylib
        private const string Lib = "meteor";

        [DllImport(Lib)] public static extern IntPtr meteor_create(ref MeteorConfig cfg);
        [DllImport(Lib)] public static extern void   meteor_destroy(IntPtr ctx);
        [DllImport(Lib)] public static extern IntPtr meteor_encode(IntPtr ctx,
                             byte[] message, UIntPtr msgLen,
                             string startingContext, out int error);
        [DllImport(Lib)] public static extern IntPtr meteor_decode(IntPtr ctx,
                             string covertext, string startingContext,
                             out UIntPtr msgLen, out int error);
        [DllImport(Lib)] public static extern void   meteor_free(IntPtr ptr);
        [DllImport(Lib)] public static extern int    meteor_llm_health(IntPtr ctx);
    }

    public sealed class Meteor : IDisposable
    {
        private IntPtr _ctx;
        private GCHandle _keyHandle;  // pin key bytes for duration of context lifetime

        // keyInput: any byte array — passphrase.GetBytes(), ECDH output, etc.
        // salt: 32-byte random value shared with the other party; null = zero salt
        public Meteor(byte[] keyInput, byte[] salt = null,
                      int beta = 3, int numCandidates = 6,
                      string llmUrl = "http://127.0.0.1:8080",
                      string hyphenDict = "data/hyph_en_US.dic")
        {
            _keyHandle = GCHandle.Alloc(keyInput, GCHandleType.Pinned);
            GCHandle saltHandle = default;
            if (salt != null) saltHandle = GCHandle.Alloc(salt, GCHandleType.Pinned);

            var cfg = new MeteorConfig {
                KeyRaw        = IntPtr.Zero,
                KeyInput      = _keyHandle.AddrOfPinnedObject(),
                KeyInputLen   = (UIntPtr)keyInput.Length,
                Salt          = salt != null ? saltHandle.AddrOfPinnedObject() : IntPtr.Zero,
                SaltLen       = (UIntPtr)(salt?.Length ?? 0),
                Beta          = beta,
                NumCandidates = numCandidates,
                LlmUrl        = llmUrl,
                HyphenDict    = hyphenDict,
                MaxSteps      = 256,
                LlmTimeoutMs  = 30000,
            };
            _ctx = MeteorNative.meteor_create(ref cfg);

            if (salt != null) saltHandle.Free();
            // Do NOT free _keyHandle here; meteor_create copies the key internally,
            // but we keep it pinned until Dispose() as a safety measure.
        }

        public string Encode(byte[] message, string startingContext)
        {
            IntPtr ptr = MeteorNative.meteor_encode(_ctx, message,
                             (UIntPtr)message.Length, startingContext, out int err);
            if (err != 0) throw new Exception($"meteor_encode error {err}");
            string result = Marshal.PtrToStringAnsi(ptr) ?? "";
            MeteorNative.meteor_free(ptr);
            return result;
        }

        public byte[] Decode(string covertext, string startingContext)
        {
            IntPtr ptr = MeteorNative.meteor_decode(_ctx, covertext, startingContext,
                             out UIntPtr msgLen, out int err);
            if (err != 0) throw new Exception($"meteor_decode error {err}");
            byte[] result = new byte[(int)msgLen];
            Marshal.Copy(ptr, result, 0, result.Length);
            MeteorNative.meteor_free(ptr);
            return result;
        }

        public void Dispose() {
            if (_ctx != IntPtr.Zero) { MeteorNative.meteor_destroy(_ctx); _ctx = IntPtr.Zero; }
            if (_keyHandle.IsAllocated) _keyHandle.Free();
        }
    }
}
```

> **Godot note:** Place `meteor.dll` / `libmeteor.so` / `libmeteor.dylib` in the Godot
> project's root or `addons/meteor/`. GDExtension is not required — pure C# P/Invoke works
> in Godot 4's .NET runtime. Reference `Meteor.cs` from any GDScript-equivalent C# node.

---

## 8. Dependencies

| Dependency | Version | License | Notes |
|---|---|---|---|
| libsodium | ≥ 1.0.18 | ISC | ChaCha20 CSPRNG + HKDF-SHA256; `apt install libsodium-dev` / vcpkg / homebrew |
| libhyphen | ≥ 2.8 | LGPL-2.1 | `apt install libhyphen-dev` / vcpkg / homebrew |
| hyph_en_US.dic | — | Apache-2.0 | Ship in `data/`; from LibreOffice dictionaries repo |
| libcurl | ≥ 7.68 | MIT-like | Almost always present; `apt install libcurl4-openssl-dev` |
| cJSON | 1.7.x | MIT | Single `.c`/`.h`; vendor in `third_party/cjson/` |
| llama.cpp | pin commit | MIT | Built as external project via CMake ExternalProject_Add; produces `llama-server` binary; not linked into the library |
| CMake | ≥ 3.18 | — | Build system |

**Optional / dev:**

| Dependency | Purpose |
|---|---|
| Unity (C testing) | Unit test framework (`third_party/unity/`) |
| valgrind | Memory leak checking |
| clang-tidy | Static analysis |

---

## 9. Build System

The build has two independent parts:

1. **The Meteor C library** — built by CMake, links libsodium / libhyphen / libcurl / cJSON.
2. **The llama.cpp server** — fetched and built as an `ExternalProject`; produces the
   `llama-server` binary that the library talks to at runtime via HTTP.

They are independent: the library does not link against llama.cpp. The server binary is
built alongside the library for convenience, but can also be installed separately.

---

### 9.1 CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.18)
project(meteor_stego C)

set(CMAKE_C_STANDARD 11)

# ── User-configurable options ─────────────────────────────────────────────────

# Pin llama.cpp to a specific commit for reproducible, deterministic inference.
# Update this deliberately when upgrading — floating-point behaviour can change.
set(LLAMA_CPP_GIT_TAG "b4693"
    CACHE STRING "llama.cpp git tag or commit hash to build llama-server from")

set(LLAMA_CPP_REPO "https://github.com/ggerganov/llama.cpp.git"
    CACHE STRING "llama.cpp repository URL")

set(METEOR_LLM_PORT "8080"
    CACHE STRING "Port llama-server listens on")

set(METEOR_LLM_SEED "42"
    CACHE STRING "RNG seed passed to llama-server (must match on encoder and decoder)")

# Path to GGUF model file — used only in the generated start script, not at build time
set(METEOR_MODEL_PATH ""
    CACHE FILEPATH "Path to the GGUF model file (optional; can be set at runtime)")

# ── System dependencies ───────────────────────────────────────────────────────

find_package(CURL REQUIRED)
find_library(HYPHEN_LIB hyphen REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED libsodium)

# ── Vendor cJSON ──────────────────────────────────────────────────────────────

add_library(cjson STATIC third_party/cjson/cJSON.c)
target_include_directories(cjson PUBLIC third_party/cjson)

# ── Meteor library ────────────────────────────────────────────────────────────

add_library(meteor SHARED
    src/meteor.c
    src/prng.c
    src/bits.c
    src/meteor_core.c
    src/syllabifier.c
    src/llm_client.c
    src/encode.c
    src/decode.c
)
target_include_directories(meteor PUBLIC include PRIVATE ${SODIUM_INCLUDE_DIRS})
target_link_libraries(meteor PRIVATE
    CURL::libcurl
    ${HYPHEN_LIB}
    ${SODIUM_LIBRARIES}
    cjson
)
target_compile_options(meteor PRIVATE ${SODIUM_CFLAGS_OTHER})
set_target_properties(meteor PROPERTIES C_VISIBILITY_PRESET hidden)
target_compile_definitions(meteor PRIVATE METEOR_BUILDING_DLL)

# ── llama.cpp server (external project) ───────────────────────────────────────
# Built separately from the library; produces the llama-server binary only.
# GPU backends (Metal, CUDA, CLBlast) are all disabled to enforce CPU-only
# deterministic inference. Enable them here only if you have verified that
# both encoder and decoder machines produce bit-identical logits on GPU.

include(ExternalProject)

ExternalProject_Add(llama_server_build
    GIT_REPOSITORY      ${LLAMA_CPP_REPO}
    GIT_TAG             ${LLAMA_CPP_GIT_TAG}
    GIT_SHALLOW         TRUE
    GIT_PROGRESS        TRUE
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=Release
        -DLLAMA_BUILD_SERVER=ON
        -DLLAMA_BUILD_TESTS=OFF
        -DLLAMA_BUILD_EXAMPLES=OFF
        -DLLAMA_METAL=OFF       # disable Apple GPU (non-deterministic vs CPU)
        -DLLAMA_CUBLAS=OFF      # disable NVIDIA CUDA
        -DLLAMA_CLBLAST=OFF     # disable OpenCL
        -DLLAMA_HIPBLAS=OFF     # disable AMD ROCm
    BUILD_BYPRODUCTS    <BINARY_DIR>/bin/llama-server
    INSTALL_COMMAND     ""      # no install; use binary in place
    EXCLUDE_FROM_ALL    FALSE   # build as part of default target
)

# Copy llama-server to build/bin/ for easy access alongside the library
ExternalProject_Get_Property(llama_server_build BINARY_DIR)

set(LLAMA_SERVER_OUTPUT "${CMAKE_BINARY_DIR}/bin/llama-server")

add_custom_command(
    OUTPUT  ${LLAMA_SERVER_OUTPUT}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${BINARY_DIR}/bin/llama-server"
            "${LLAMA_SERVER_OUTPUT}"
    DEPENDS llama_server_build
    COMMENT "Copying llama-server to build/bin/"
)
add_custom_target(copy_llama_server ALL
    DEPENDS ${LLAMA_SERVER_OUTPUT}
)

# ── Generate start script from CMake variables ────────────────────────────────
# Produces build/start_llama_server.sh (and .bat on Windows) with the correct
# port, seed, and model path baked in. Edit the script or re-run cmake to
# change these values.

configure_file(
    "${CMAKE_SOURCE_DIR}/scripts/start_llama_server.sh.in"
    "${CMAKE_BINARY_DIR}/start_llama_server.sh"
    @ONLY
)
configure_file(
    "${CMAKE_SOURCE_DIR}/scripts/start_llama_server.bat.in"
    "${CMAKE_BINARY_DIR}/start_llama_server.bat"
    @ONLY
)
file(CHMOD "${CMAKE_BINARY_DIR}/start_llama_server.sh"
     PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                 GROUP_READ GROUP_EXECUTE
                 WORLD_READ WORLD_EXECUTE)

# ── Helper targets ────────────────────────────────────────────────────────────

# cmake --build build --target start_server
# Launches llama-server in the background; requires METEOR_MODEL_PATH to be set.
if(NOT METEOR_MODEL_PATH STREQUAL "")
    add_custom_target(start_server
        COMMAND "${CMAKE_BINARY_DIR}/start_llama_server.sh" "${METEOR_MODEL_PATH}"
        DEPENDS copy_llama_server
        COMMENT "Starting llama-server on port ${METEOR_LLM_PORT}..."
        USES_TERMINAL
    )
else()
    add_custom_target(start_server
        COMMAND ${CMAKE_COMMAND} -E echo
                "Set -DMETEOR_MODEL_PATH=<path/to/model.gguf> to use this target"
        COMMENT "METEOR_MODEL_PATH not set"
    )
endif()

# cmake --build build --target server_health
# Checks that a running llama-server is reachable on the configured port.
add_custom_target(server_health
    COMMAND curl --silent --fail --max-time 3
            "http://127.0.0.1:${METEOR_LLM_PORT}/health"
            -o /dev/null
            && ${CMAKE_COMMAND} -E echo "llama-server is healthy on port ${METEOR_LLM_PORT}"
            || ${CMAKE_COMMAND} -E echo "llama-server not reachable on port ${METEOR_LLM_PORT}"
    COMMENT "Checking llama-server health..."
    USES_TERMINAL
)

# ── Tests ─────────────────────────────────────────────────────────────────────

enable_testing()
foreach(test prng bits meteor_core syllabifier roundtrip determinism)
    add_executable(test_${test} tests/test_${test}.c)
    target_link_libraries(test_${test} meteor)
    add_test(NAME ${test} COMMAND test_${test})
endforeach()

# test_roundtrip and test_determinism need a live server; skip them if not reachable
set_tests_properties(roundtrip determinism PROPERTIES
    SKIP_REGULAR_EXPRESSION "llama-server not reachable"
)
```

---

### 9.2 Startup script template

**`scripts/start_llama_server.sh.in`** (configure_file fills in `@VARIABLES@`):

```bash
#!/usr/bin/env bash
# Auto-generated by CMake — do not edit directly.
# Re-run cmake with -DMETEOR_MODEL_PATH=... to regenerate.
#
# Starts llama-server with settings required for deterministic Meteor operation.
# BOTH the encoder machine and the decoder machine must use these exact settings
# and the same model file (verified by SHA-256).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LLAMA_SERVER="${SCRIPT_DIR}/bin/llama-server"

# Allow model path override at runtime: ./start_llama_server.sh /path/to/model.gguf
MODEL_PATH="${1:-@METEOR_MODEL_PATH@}"
PORT="@METEOR_LLM_PORT@"
SEED="@METEOR_LLM_SEED@"
LOGFILE="${SCRIPT_DIR}/llama_server.log"
PIDFILE="${SCRIPT_DIR}/llama_server.pid"

if [[ ! -f "$LLAMA_SERVER" ]]; then
    echo "ERROR: llama-server not found at $LLAMA_SERVER" >&2
    echo "Run: cmake --build build --target copy_llama_server" >&2
    exit 1
fi

if [[ -z "$MODEL_PATH" || ! -f "$MODEL_PATH" ]]; then
    echo "ERROR: model file not found: ${MODEL_PATH:-<not set>}" >&2
    echo "Usage: $0 /path/to/model.gguf" >&2
    exit 1
fi

# Optional: verify model SHA-256 matches expected hash.
# Uncomment and fill in EXPECTED_SHA256 after choosing your model.
# EXPECTED_SHA256="<sha256sum of your model.gguf>"
# ACTUAL_SHA256="$(sha256sum "$MODEL_PATH" | awk '{print $1}')"
# if [[ "$ACTUAL_SHA256" != "$EXPECTED_SHA256" ]]; then
#     echo "ERROR: model file hash mismatch." >&2
#     echo "  expected: $EXPECTED_SHA256" >&2
#     echo "  actual:   $ACTUAL_SHA256" >&2
#     echo "Both encoder and decoder must use the same model file." >&2
#     exit 1
# fi

# Kill any existing instance on this port
if [[ -f "$PIDFILE" ]]; then
    OLD_PID="$(cat "$PIDFILE")"
    if kill -0 "$OLD_PID" 2>/dev/null; then
        echo "Stopping existing llama-server (PID $OLD_PID)..."
        kill "$OLD_PID"
        sleep 1
    fi
    rm -f "$PIDFILE"
fi

echo "Starting llama-server..."
echo "  model:   $MODEL_PATH"
echo "  port:    $PORT"
echo "  seed:    $SEED"
echo "  threads: 1 (required for determinism)"
echo "  log:     $LOGFILE"

"$LLAMA_SERVER"           \
    --model   "$MODEL_PATH" \
    --port    "$PORT"       \
    --host    127.0.0.1     \
    --threads 1             \
    --seed    "$SEED"       \
    --temp    0.0           \
    --ctx-size 2048         \
    --no-mmap               \
    --log-disable           \
    >> "$LOGFILE" 2>&1 &

echo $! > "$PIDFILE"
echo "llama-server started (PID $(cat "$PIDFILE"))"

# Wait for server to become healthy (up to 30 seconds)
for i in $(seq 1 30); do
    if curl --silent --fail --max-time 1 \
            "http://127.0.0.1:${PORT}/health" -o /dev/null 2>/dev/null; then
        echo "llama-server is ready."
        exit 0
    fi
    sleep 1
done
echo "WARNING: llama-server did not become healthy within 30 seconds." >&2
echo "Check $LOGFILE for errors." >&2
exit 1
```

**`scripts/start_llama_server.bat.in`** (Windows equivalent):

```batch
@echo off
REM Auto-generated by CMake — do not edit directly.
REM Starts llama-server with deterministic settings for Meteor.

set SCRIPT_DIR=%~dp0
set LLAMA_SERVER=%SCRIPT_DIR%bin\llama-server.exe
set MODEL_PATH=%~1
if "%MODEL_PATH%"=="" set MODEL_PATH=@METEOR_MODEL_PATH@
set PORT=@METEOR_LLM_PORT@
set SEED=@METEOR_LLM_SEED@
set LOGFILE=%SCRIPT_DIR%llama_server.log

if not exist "%LLAMA_SERVER%" (
    echo ERROR: llama-server.exe not found at %LLAMA_SERVER%
    echo Run: cmake --build build --target copy_llama_server
    exit /b 1
)
if not exist "%MODEL_PATH%" (
    echo ERROR: model file not found: %MODEL_PATH%
    echo Usage: start_llama_server.bat path\to\model.gguf
    exit /b 1
)

echo Starting llama-server...
echo   model:   %MODEL_PATH%
echo   port:    %PORT%
echo   seed:    %SEED%
echo   threads: 1

start /B "" "%LLAMA_SERVER%" ^
    --model   "%MODEL_PATH%" ^
    --port    %PORT%          ^
    --host    127.0.0.1       ^
    --threads 1               ^
    --seed    %SEED%          ^
    --temp    0.0             ^
    --ctx-size 2048           ^
    --no-mmap                 ^
    --log-disable             ^
    >> "%LOGFILE%" 2>&1

echo llama-server launched. Check %LOGFILE% if issues arise.
```

---

### 9.3 Build commands

```bash
# Configure — set model path if known at build time (optional)
cmake -B build -DCMAKE_BUILD_TYPE=Release \
               -DLLAMA_CPP_GIT_TAG=b4693   \
               -DMETEOR_LLM_PORT=8080       \
               -DMETEOR_LLM_SEED=42         \
               -DMETEOR_MODEL_PATH=/path/to/model.gguf

# Build the library and fetch+build llama-server (this will take a while the first time)
cmake --build build

# Start the server (uses METEOR_MODEL_PATH from cmake configure, or pass path manually)
./build/start_llama_server.sh /path/to/model.gguf

# Verify server is alive
cmake --build build --target server_health

# Run all tests (roundtrip/determinism require a live server on port 8080)
ctest --test-dir build --output-on-failure
```

> **Note on build time:** `ExternalProject_Add` fetches and compiles llama.cpp on the first
> build, which can take 5–15 minutes. Subsequent builds only recompile if `LLAMA_CPP_GIT_TAG`
> changes. The server binary is not rebuilt when you modify the Meteor library source.

---

## 10. Error Handling

All public API functions return `NULL` (pointer-returning) or write to `*out_error`
(int-returning) on failure. Internal functions propagate errors upward via return codes
— no `exit()` or `abort()` calls inside the library.

**LLM error recovery:** If the LLM call fails (network error, timeout, malformed JSON),
`llm_client_get_syllable_dist` returns a uniform distribution over the candidate syllables
as a fallback. This will cause bit errors but avoids crashing. Set `METEOR_ERR_LLM` in
`out_error` and log a warning. The caller decides whether to abort or continue.

**Thread safety:** `MeteorCtx` is not thread-safe. Each thread must use its own context.
The library has no global mutable state.

---

## 11. Testing Strategy

| Test | What it verifies |
|---|---|
| `test_prng` | ChaCha20 keystream matches known-answer test vectors from RFC 8439; `prng_next_bits` produces correct MSB-first bit extraction; state advances consistently across block boundaries |
| `test_key_derivation` | HKDF-SHA256 output matches reference vectors; different inputs produce different keys; same input + same salt always produces same key; `prng_wipe` zeroes all key bytes |
| `test_bits` | Round-trip: bytes → bits → bytes for all 256 byte values; null terminator detection |
| `test_meteor_core` | Slot building sums to 2^beta; encode/decode recovers same bits for mock distributions |
| `test_syllabifier` | Known word list against expected syllabification; libhyphen vs heuristic agreement |
| `test_roundtrip` | Full encode → decode with live LLM; recovered message == original message |
| `test_determinism` | Two calls to LLM with same prompt produce slot-equivalent distributions |

**PRNG known-answer test (RFC 8439 §2.1.1 test vector):**

```c
void test_chacha20_vector(void) {
    // RFC 8439 test vector: known key, nonce, counter → known first 64 bytes
    uint8_t key[32] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
        0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
        0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f
    };
    uint8_t nonce[8] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x4a};
    uint8_t zeros[64] = {0};
    uint8_t stream[64];

    crypto_stream_chacha20_xor_ic(stream, zeros, 64, nonce, 1, key);

    // First 4 bytes of expected output from RFC 8439
    assert(stream[0] == 0x22);
    assert(stream[1] == 0x4f);
    assert(stream[2] == 0x51);
    assert(stream[3] == 0xf3);
}
```

**Key derivation test:**

```c
void test_hkdf_determinism(void) {
    uint8_t key_a[32], key_b[32];
    const uint8_t input[]  = "test-passphrase";
    const uint8_t salt[32] = {0x42}; // non-zero salt

    MeteorPRNG p1, p2;
    prng_init(&p1, input, sizeof(input)-1, salt, 32);
    prng_init(&p2, input, sizeof(input)-1, salt, 32);

    // Same input + salt must always produce identical first 64 output bits
    assert(prng_next_bits(&p1, 32) == prng_next_bits(&p2, 32));
    assert(prng_next_bits(&p1, 32) == prng_next_bits(&p2, 32));

    // Different passphrase must produce different stream
    MeteorPRNG p3;
    const uint8_t other[] = "different-passphrase";
    prng_init(&p3, other, sizeof(other)-1, salt, 32);
    // (statistical test — first 32 bits will almost certainly differ)
    // Run 1000 times with random inputs to confirm; omitted here for brevity
    prng_wipe(&p1); prng_wipe(&p2); prng_wipe(&p3);
}

void test_prng_wipe(void) {
    MeteorPRNG p;
    uint8_t input[] = "secret";
    prng_init(&p, input, sizeof(input)-1, NULL, 0);
    prng_next_bits(&p, 8); // advance state
    prng_wipe(&p);

    // All bytes of the struct must be zero after wipe
    uint8_t* raw = (uint8_t*)&p;
    for (size_t i = 0; i < sizeof(MeteorPRNG); i++)
        assert(raw[i] == 0);
}
```

**Roundtrip test approach:**

```c
// Use a short message and verify exact recovery
const uint8_t key_input[] = "my-shared-secret-passphrase";
const uint8_t salt[32]    = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
                              17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32};
const char* message = "hi";
const char* context = "The report stated";

MeteorConfig cfg = {
    .key_raw       = NULL,
    .key_input     = key_input,
    .key_input_len = sizeof(key_input) - 1,
    .salt          = salt,
    .salt_len      = 32,
    .beta          = 3,
    /* … other fields … */
};
MeteorCtx* ctx = meteor_create(&cfg);

int err;
char* covertext = meteor_encode(ctx, (uint8_t*)message, strlen(message), context, &err);
assert(err == METEOR_OK);

size_t   recovered_len;
uint8_t* recovered = meteor_decode(ctx, covertext, context, &recovered_len, &err);
assert(err == METEOR_OK);
assert(recovered_len == strlen(message));
assert(memcmp(recovered, message, recovered_len) == 0);

meteor_free(covertext);
meteor_free(recovered);
meteor_destroy(ctx);
```

---

## 12. Known Limitations & Future Work

**Current limitations:**

- **CPU-only determinism:** GPU inference may produce different float values across
  hardware vendors. Enforce `--gpu-layers 0` on both sides until cross-GPU determinism
  is verified.
- **English only:** libhyphen dictionary is English. Extend by loading language-specific
  `.dic` files.
- **No error correction:** A single wrong bit from a mismatched distribution corrupts
  all bytes after it. Consider wrapping the message in a Reed-Solomon or Hamming code
  before passing to `meteor_encode`.
- **LLM latency:** Each syllable step is one HTTP round-trip (~50–200ms locally).
  A 10-word covertext with ~3 syllables/word = ~30 API calls = 1.5–6 seconds. This is
  acceptable for an offline tool but not for real-time use.
- **Context length:** Very long covertexts may exceed the model's context window. Monitor
  `full_text` length; truncate or summarise earlier context if needed.

**Future work:**

- Batch LLM calls (request distributions for N syllable steps in one prompt)
- Error-correcting code wrapper
- Multi-language support via language-detected hyphen dictionary
- llama.cpp linked-library backend (eliminates HTTP overhead, improves determinism)
- Subword tokeniser alignment (map model's BPE tokens to syllable boundaries directly)
