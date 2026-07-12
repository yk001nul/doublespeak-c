# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

**doublespeak-c** is a C library implementing a syllable-level variant of the Meteor steganographic system (Kaptchuk et al., CCS 2021). It encodes binary data into LLM-generated covertext and recovers it with a shared symmetric key. The full target architecture is specified in `ARCHITECTURE.md` — that document is authoritative for what to build.

The `doublespeak/` directory currently contains a Visual Studio CMake scaffold (hello-world placeholder). The implementation should follow the directory layout in `ARCHITECTURE.md §3`.

## Projects

| Directory | Purpose |
|---|---|
| `meteor_stego/` | The Meteor stego C library (main implementation), plus the `doublespeak` CLI (`meteor_stego/apps/`) that exercises it end-to-end |
| `doublespeak/` | Original VS-generated C++ scaffold (hello-world placeholder, not the library, and not the CLI — see `meteor_stego/apps/doublespeak.c`) |

## Windows toolchain paths

The VS 2022 Community tools are NOT on PATH by default. Every build session must start with `vcvarsall.bat x64`. Exact paths on this machine:

| Tool | Path |
|---|---|
| `vcvarsall.bat` | `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat` |
| `cmake.exe` | `C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` |
| `ninja.exe` | `C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe` |
| `curl.exe` (downloads) | `C:\Program Files\Git\mingw64\bin\curl.exe` — use `--insecure` flag; system SSL certs are broken on this machine |

**vcpkg is blocked on this machine** — `vcpkg install` fails with SSL connect errors when downloading its own bootstrap and packages. Do not attempt it. Dependencies are vendored instead (see below).

## Build — meteor_stego

CMake presets are in `meteor_stego/CMakePresets.json`. The presets hardcode the VS 2022 Community paths above. VS Code reads them automatically via CMake Tools (`.vscode/settings.json` inside `meteor_stego/`).

**Available presets by platform:**

| Platform | Configure preset | Build preset | Test preset |
|---|---|---|---|
| Windows x64 Debug | `x64-debug` | `x64-debug-build` | `x64-debug-test` |
| Windows x64 Release | `x64-release` | `x64-release-build` | — |
| Linux Debug | `linux-debug` | `linux-debug-build` | `linux-debug-test` |
| Linux Release | `linux-release` | `linux-release-build` | — |
| macOS Debug (native arch) | `macos-debug` | `macos-debug-build` | `macos-debug-test` |
| macOS Release (native arch) | `macos-release` | `macos-release-build` | `macos-release-test` |
| macOS arm64 Debug | `macos-arm64-debug` | `macos-arm64-debug-build` | — |
| macOS arm64 Release | `macos-arm64-release` | `macos-arm64-release-build` | — |

macOS presets set `CMAKE_OSX_DEPLOYMENT_TARGET=12.0` (minimum supported macOS version).

**Windows: no dependency installation needed** — libsodium is vendored as prebuilt MSVC static libs in `meteor_stego/deps/libsodium/`. WinHTTP (Windows-native) is used instead of libcurl. cJSON is vendored in `meteor_stego/third_party/cjson/`.

**Linux/macOS — install system packages first:**
```
apt install libsodium-dev libcurl4-openssl-dev libhyphen-dev   # Debian/Ubuntu
brew install libsodium curl hyphen                              # macOS
```

**Configure and build** (from repo root):

```powershell
# Windows — must activate MSVC environment first, then call the bundled cmake
$vcvars = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat'
$cmake  = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$build  = 'E:\Development\doublespeak-c\meteor_stego\out\build\x64-debug'

# Configure (only needed once or after CMakeLists changes)
cmd /c "call `"$vcvars`" x64 && `"$cmake`" --preset x64-debug -S meteor_stego"

# Build
cmd /c "call `"$vcvars`" x64 && `"$cmake`" --build `"$build`" --parallel"
```

```bash
# Linux
cmake --preset linux-debug -S meteor_stego
cmake --build meteor_stego/out/build/linux-debug

# macOS (Intel or Apple Silicon — builds for the host architecture)
cmake --preset macos-debug -S meteor_stego
cmake --build meteor_stego/out/build/macos-debug

# macOS — explicit arm64 (Apple Silicon; also works on Intel via Rosette for CI)
cmake --preset macos-arm64-debug -S meteor_stego
cmake --build meteor_stego/out/build/macos-arm64-debug
```

**Build llama-server** (optional; needed for roundtrip/determinism tests):

```powershell
cmd /c "call `"$vcvars`" x64 && `"$cmake`" --build `"$build`" --target copy_llama_server"
```

**Run all tests**:

```powershell
# Windows — ctest is co-located with cmake
cmd /c "call `"$vcvars`" x64 && ctest --test-dir `"$build`" --output-on-failure"

# Single test suite by name regex
cmd /c "call `"$vcvars`" x64 && ctest --test-dir `"$build`" --output-on-failure -R prng"
```

```bash
# Linux
ctest --preset linux-debug-test

# macOS
ctest --preset macos-debug-test
```

## Runtime: llama-server

The library communicates with a locally running `llama-server` (llama.cpp) via HTTP on port 8080. Start it before running encode/decode or roundtrip tests:

```bash
# Scripts are generated by cmake configure_file from scripts/*.in templates
meteor_stego/out/build/linux-debug/start_llama_server.sh /path/to/model.gguf
meteor_stego/out/build/macos-debug/start_llama_server.sh /path/to/model.gguf
meteor_stego\out\build\x64-debug\start_llama_server.bat  path\to\model.gguf

# Verify health
cmake --build meteor_stego/out/build/x64-debug --target server_health
```

Critical server flags for determinism: `--threads 1 --temp 0.0 --seed 42 --no-mmap` (`METEOR_NUM_THREADS` overrides the thread count — see "Performance tuning" below; encoder and decoder must match). The start scripts also pass `--parallel 1 --no-cont-batching` (single decode slot, no cross-request batching — determinism hardening). See `ARCHITECTURE.md §6` for the full determinism checklist.

## Performance tuning (`imp/perf-opt-2` branch)

Essentially all encode/decode wall-clock time is LLM inference — one or more blocking HTTP round-trips to `llama-server` per Meteor step, 8–20+ steps per short message. Two validated levers:

1. **HTTP keep-alive (always on, zero determinism impact).** `llm_client.c` reuses one persistent WinHTTP session+connection (and `CURLOPT_TCP_KEEPALIVE` on the libcurl backend) instead of opening/closing a socket per request. Payloads are unchanged, so this cannot affect determinism; it just removes a TCP connect+teardown per step. A stale keep-alive socket is transparently reconnected and retried once. Validated: the full `determinism`/`roundtrip`/`styled_encode`/`doublespeak_cli`/`capacity` ctest suite passes 100% on Phi-3.5-mini at `--threads 1`.

2. **Thread count — `METEOR_NUM_THREADS` env var (default 1).** The start scripts read it and pass `--threads N`. llama.cpp's CPU backend is reproducible for a *fixed* thread count, so raising this is a near-linear speedup — but the count is now part of the shared protocol: **encoder and decoder must use the identical value**, and cross-machine use is only safe when both machines run the same count. Default stays 1 so existing behavior is unchanged. Validate any new value with `ctest -R "determinism|roundtrip|styled_encode"` before trusting it. Validated so far on this machine (Phi-3.5-mini): `--threads 1` and `--threads 4` — the latter passed the full 5-test suite 100% (2026-07-12) with a ~2.6× overall speedup (roundtrip 3222s→1332s, styled_encode 6242s→2303s, doublespeak_cli 1444s→544s).

### Rejected: prompt caching (`cache_prompt` / KV reuse) — breaks determinism

An earlier revision of this branch added an opt-in `METEOR_PROMPT_CACHE` mode that set `"cache_prompt": true` + `"id_slot": 0` and erased the slot at the start of each run, aiming to turn per-run prefill from ≈O(steps²) into ≈O(steps). **It was removed after failing validation.** With `METEOR_PROMPT_CACHE=1` on Phi-3.5-mini (server at `--parallel 1 --no-cont-batching`, slot erased per run), `roundtrip`, `capacity`, and 3 of 4 `styled_encode` styles passed, but `styled_encode`'s NEWS_ARTICLE case and the `doublespeak_cli` direct style-mode roundtrip **desynced** — the covertext diverged mid-generation and decode recovered the wrong message/length. llama-server's partial-prefix KV reuse yields floating-point-divergent logits at some step, which is enough to break the encode/decode lockstep intermittently (silently, per the determinism constraint below). The *same* suite passes 100% with caching off. Do not re-enable prompt caching without a fundamentally different, bit-exact caching mechanism. The prior (unmerged) `imp/perf-opt` branch reached the same conclusion and kept `cache_prompt` off.

## Architecture

The library is structured around 7 components (see `ARCHITECTURE.md §4` for full specs):

| Component | Files | Role |
|---|---|---|
| PRNG | `src/prng.c/.h` | ChaCha20 CSPRNG keyed via HKDF-SHA256 (libsodium) |
| Bit Packing | `src/bits.c/.h` | Message ↔ bit array, MSB-first, null-terminated |
| Meteor Core | `src/meteor_core.c/.h` | One encode/decode step: slot table + common-prefix recovery |
| Syllabifier | `src/syllabifier.c/.h` | libhyphen wrapper + heuristic fallback (not used in decode — see below) |
| LLM Client | `src/llm_client.c/.h` | WinHTTP (Windows) / libcurl (Linux/macOS) + cJSON; two-grammar GBNF: `NEW_WORD_GRAMMAR` (syllables only) for word starts, `CONTINUATION_GRAMMAR` (syllables + mandatory `·` EOW at end) for continuations |
| Encode | `src/encode.c/.h` | Full encode pipeline; loop runs until all message bits are encoded **and** the current word ends with EOW (not just until bits are exhausted) |
| Decode | `src/decode.c/.h` | Full decode pipeline; uses **LLM prefix-matching** to recover the encoder's syllable sequence from each covertext word — the syllabifier is not used |

Public API is in `include/meteor.h`. FFI bindings (Python ctypes, C# P/Invoke) live in `bindings/`.

**Key invariant:** Encoder and decoder must produce byte-identical LLM prompts at every step. The reconstructed `full_text` string must use exactly the same space separators, word boundaries, and `starting_context` prefix on both sides. Any divergence corrupts all subsequent bit recovery with no error signal.

### Encode/decode algorithm notes

**Two-grammar system:** With `--temp 0.0` the model is greedy and assigns near-zero probability to the EOW token `·` when it is merely *allowed* by the grammar. `CONTINUATION_GRAMMAR` makes `·` *mandatory* as the final key in every continuation response, guaranteeing it always appears in the slot table.

**Encoder loop termination:** The encode loop condition is `(bit_offset < total_bits || partial_word[0] != '\0')`. The encoder must always finish the current word with an EOW step before stopping, because the decoder synthesises an EOW step at every word boundary. Stopping mid-word (old behaviour) caused a one-step PRNG divergence per word.

**Decoder prefix-matching:** The syllabifier cannot reconstruct the encoder's syllable sequence for artificially concatenated words (e.g. `"resreinin..."` — heuristic VC|CV splits differ from the encoder's actual LLM choices). Instead, the decoder iterates each covertext word character by character, querying the LLM with the same context/partial as the encoder, and picks the longest candidate that is a prefix of the remaining text. After all syllables of a word are consumed, one EOW synthesis step is run to keep the PRNG in sync — unless the null terminator was already found mid-word (in which case EOW synthesis is skipped, matching the encoder which also had no EOW for the last partial word).

### Style mode (phrase-level paraphrase encoding, `imp/topic-gen` branch)

Style mode (`MeteorConfig.style`, one of `MeteorStyle`) replaces syllable-level word building with phrase-level paraphrase encoding: each step picks one of `num_candidates` LLM-generated 3-6 word phrases (`beta=3` / 8 candidates fills all slots exactly, 3 bits/step) and appends it to a growing sentence, instead of building words syllable-by-syllable. Same lockstep-PRNG invariant as syllable mode applies — encode.c and decode.c must draw from the PRNG in the exact same order every step, regardless of branch outcomes, or the streams desync with no error signal.

Per-step PRNG draws, in fixed order:
1. `meteor_draw_style_question()` — picks one of 5 `StyleQuestion` axes (HOW/WHERE/WHO_MEET/WHO_AVOID/WHY) that all 8 candidates for this step answer, giving them a shared semantic axis instead of open-ended "continue naturally" (which produced low-quality filler candidates). Grammar-hardened via `build_phrase_grammar` in `llm_client.c` so each candidate's connector (e.g. "by ...", "to ...") is enforced by GBNF, not just prompted.
2. `meteor_draw_clause_end()` — decides whether the phrase about to be generated ends the current sentence (append a literal `.`, reset `subject_anchor`, start fresh). Forced to continue below `CLAUSE_END_MIN_PHRASES=2` and forced to end at `CLAUSE_END_MAX_PHRASES=2`, but the PRNG bits are always drawn regardless of which bound fires, to keep stream position identical between encode/decode.
3. `meteor_draw_digress_mode()` / `meteor_draw_digression_axis()` / `meteor_draw_digression_variant()` — decide whether the sentence about to be opened digresses onto a secondary entity instead of paraphrasing the topic, which `DigressionAxis` it asks about, and which of `DIGRESS_VARIANT_COUNT` phrasings of that axis's question to use. See "Topic subordination (digression)" below.
4. The beta-bit slot-selection draw inside `meteor_encode_step`/decode's mirror, same as syllable mode.

All of the above draws happen unconditionally every step, including the very first phrase of the covertext (where the question/digression draws are unused) — this is required so PRNG position never depends on data-dependent branches.

**Subject anchor:** the first phrase of each sentence (`subject_anchor[0]=='\0'` on entry) is prompted to open with an explicit subject (pronoun or noun phrase), and that subject is repeated verbatim in every later phrase-dist prompt for the rest of the sentence to keep person/tense consistent. `subject_anchor` (not `ctx_len`/`full_context`) is what `build_phrase_prompt` and `llm_client_get_phrase_dist` key off to decide opening-step vs. continuation-step grammar — `full_context` stays non-empty across sentence boundaries, so it can't be used to detect "first phrase of *this* sentence."

**Anti-repetition:** `MeteorWordHistory` (shared helper in `meteor_core.c/.h`, used identically by encode.c and decode.c so both sides build byte-identical prompts) blacklists the last 8 non-stopword tokens; `PHRASE_HISTORY` blacklists the last 6 whole phrases. Neither resets at sentence boundaries — repeats are suppressed across the entire message, not just within one sentence.

**Decoder note:** the literal `.` inserted at a clause end has no space before it in the covertext and isn't part of any LLM candidate's text, so decode.c must consume it explicitly (`if (*remaining == '.') remaining++`) right after matching the phrase that preceded it, before resuming its normal space-skip.

**Topic subordination (digression):** every sentence used to paraphrase the same fixed topic (`starting_context`, quoted verbatim in `preamble`'s "Original: ..." line for the whole message), which reads as repetitive/implausible for a short context and a long message. `meteor_draw_digress_mode()` (`meteor_core.c`) decides, at each sentence-opening step (`subject_anchor[0]=='\0'`), whether that sentence instead digresses onto a secondary subject/object from the text so far. Digression is gated behind `DIGRESS_MIN_TOPIC_SENTENCES=1` (the opening sentence is always topic-anchored) and capped at one hop via `DigressionState.last_was_digression` (a digression sentence is always followed by a forced topic-anchored sentence) — both checked *after* the PRNG bits are drawn, same forced-bound pattern as `meteor_draw_clause_end`. `meteor_draw_digression_axis()` independently picks one of 5 `DigressionAxis` values (DESCRIBE/STATE/SIGNIFICANCE/ORIGIN/OUTCOME); `meteor_draw_digression_variant()` picks one of `DIGRESS_VARIANT_COUNT` alternate phrasings of that axis's question, purely for wording variety. All three draws happen every step (like `StyleQuestion`) but are only consulted — and only latched into `sentence_is_digression`/`sentence_axis`/`sentence_variant` for later use at the sentence's clause-end — on opening steps.

A digression opening step is a **two-stage** process, not a single LLM call: (1) `llm_client_get_digression_answer()` (`llm_client.c`) asks `DIGRESSION_QUESTION[axis][variant]` — a plain, non-bit-embedding, no-grammar `/completion` call (temp 0, seed 42) about a secondary entity picked by the model from the text so far — and returns its one-sentence answer; this call carries no message-payload bits, it only establishes what to talk about, and both encode.c/decode.c get byte-identical text since it's deterministic (falling back to a fixed local string, `DIGRESSION_FALLBACK_ANSWER`, on HTTP/parse failure so lockstep survives a server hiccup). (2) that answer is fed into `llm_client_build_preamble()` to build a one-off temporary preamble ("Paraphrase the sentence below... Original: `<answer>`") which is passed to `llm_client_get_phrase_dist()` **in place of** the main topic preamble for this one opening-step call — taking the exact same code path (`OPENING_SUBJECT_GRAMMAR`, beta-bit slot selection) as a normal topic-anchored opening, so real message bits still get embedded in whichever paraphrase-of-the-answer is selected. The raw question is never written to the covertext; only the paraphrased answer is. Continuation steps of that sentence are unaffected — still `STYLE_QUESTION_PHASE`/`build_phrase_grammar` against the main preamble, relying on `subj_clause` (naming the digression-derived opener verbatim) to keep them anchored to the digression's subject rather than the main topic.

**Status:** the bare-verb sentence-opener issue (above) was fixed via `OPENING_SUBJECT_GRAMMAR` (commit `0494509`, `imp/topic-subordination`). Topic subordination (digression, above) is implemented and committed (commit `2bfb42f`, `imp/topic-subordination`), verified with a full `roundtrip`/`determinism`/`capacity`/`styled_encode` ctest rerun (100% pass, all 4 styles) on a freshly-restarted `llama-server`. That same verification pass also turned up and fixed two independent bugs unrelated to digression itself, both included in commit `2bfb42f`:
- **`meteor_build_dist` slot-allocation overflow** (`meteor_core.c`): the largest-remainder slot allocator forced every near-zero-probability candidate up to a minimum of 1 slot *before* the top-up-to-`total_slots` pass, which could push the total above `total_slots` and leave trailing candidates with out-of-range slot ranges. Fixed by reordering to floor → top-up-to-total → steal-for-minimums, which can never overflow.
- **llama-server prompt-cache nondeterminism** (`llm_client.c`): none of the 4 `/completion` request builders set `cache_prompt`, so llama-server's default cache reuse made an identical request return different completions depending on the server's prior unrelated request history — confirmed by rerunning the same deterministic test twice on the same warm server (identical output both times) versus once more after the server had processed a long unrelated request history first (different, failing output). Fixed by setting `"cache_prompt": false` on every request; see the Determinism constraint section below.

Known remaining rough edge (not a correctness bug, deferred): subject placement within a digression/subordinate sentence sometimes reads awkwardly (the digression's own subject can end up oddly positioned relative to the main clause) — noted for future work, not yet scheduled.

Not merged to `main` yet.

### Coherence pass (`imp/text-coherence-opt` branch)

A follow-up pass targeting style-mode covertext *plausibility* (independent of the digression work above). Two prior symptoms were diagnosed and fixed; both changes are prompt/parameter-side only (no PRNG/protocol change — encode.c and decode.c share the same compiled-in builders, so lockstep is preserved), and each was validated with `styled_encode` 28/28 at threads=4 on Phi-3.5-mini:

- **Topic drift / cross-topic vocabulary bleed** (commit `1a4869a`): the few-shot example phrases inside `STYLE_QUESTION_PHASE` and the opening-subject phase (`llm_client.c`) were all commute/office-themed ("by turning the key", "to the office", "his car"); at `temp=0.0` they dominated the output domain and leaked that vocabulary into *every* style regardless of topic (a hiking blog produced "carpooling"/"department head"). Fixed by de-theming the examples to grammatical-shape-only placeholders ("by <the means>") plus an explicit "draw vocabulary from the topic" instruction. Connector words are unchanged, so the `STYLE_QUESTION_CONNECTOR_GRAMMAR` sync invariant still holds.
- **Word-salad run-ons** (commits `1a4869a` then `72a327c`): a sentence stacked up to 5 PRNG-selected prepositional modifiers ("to avoid X to Y by Z with W"). `CLAUSE_END_MAX_PHRASES` was lowered 6→3→**2**, so a sentence is now an opener plus at most one modifier and unrelated modifiers can no longer pile up.

**Fixed in a follow-up branch:**
- **Verbless / fragment openers** — resolved on `imp/text-verblessness-opt` (commit `6e655d2`); see "Verblessness pass" below.

**Still deferred (known non-blocking quality issues, not correctness bugs):**
- **Style-register bleed** — all four styles read in a similar register; a `FORMAL_EMAIL` doesn't read more formal than an `INFORMAL_CHAT`. (Next scheduled work.)
- **Pronoun-referent drift** — a residual of the verblessness fix (pronoun-only openers, below): the subject pronoun can jump across sentences within one covertext (he→they→she) and occasionally mismatch ("they run using his vehicle"). Each sentence is individually well-formed; the drift is a cross-sentence coherence nit.
- Occasional covertext truncation mid-word ("environmen.") when a short message spans more/shorter sentences.

### Verblessness pass (`imp/text-verblessness-opt` branch)

Fixes the verbless/fragment-opener issue deferred above (which also supersedes the older digression Status note's "fixed" claim — that fixed *bare-verb*/missing-subject openers, not missing-verb fragments). `OPENING_SUBJECT_GRAMMAR` (`llm_client.c`) previously forced only a subject-first opener (`subject (" " word)+`), which let the model slide from the subject straight into a preposition ("that to office.", "the process has seen with key partners."). The grammar now requires a `verb` token immediately after the subject (`subject " " verb (" " word)*`), where `verb` is a broad closed list of common verbs (3rd-person-singular + base forms plus copulas/auxiliaries/modals). Subject is restricted to **pronouns only**: forcing a verb after a determiner+noun subject created two new failures — wrong-agent nonsense ("the mountains climb") and noun-phrase openers when the verb slot hit a homograph ("the quarterly report ...") — both seen in CLI eyeball encodes and both absent with pronoun subjects. Determiner openers ("the team presents") reappear as "it/they present", which reads just as well. Compiled-in grammar, identical on both sides, so the encode/decode lockstep holds. Validated `styled_encode` 28/28 at threads=4 on Phi-3.5-mini (commit `6e655d2`).

### Sample style-mode outputs (verified 2026-07-13, commit `6e655d2`, branch `imp/text-verblessness-opt`, threads=4)

All four covertexts below successfully round-tripped (`meteor_decode` recovered the exact original message) in the verification ctest run. Kept here for reference so the styles' output character can be checked without re-running the (slow, LLM-backed) test suite — only re-run `styled_encode` if a change could plausibly affect phrase/candidate generation, grammar, or the digression logic.

| Style | Topic (starting context) | Message | Covertext output |
|---|---|---|---|
| `INFORMAL_CHAT` | John goes to the office using his car every morning. | `hi` | he heads to the office to avoid meeting tight deadlines. they run using his vehicle. he takes the subway to meet the new intern. she takes the train with colleagues. he drives to work to avoid being late. he spends the drive. |
| `NEWS_ARTICLE` | The government announced new policies to reduce carbon emissions by 2030. | `hi` | they establish to avoid energy shortages. she ensures by implementing green technologies. they ensure to join climate action coalition. they adopt new strategies to join global sustainability forums. they introduce to avoid environmental degradation. he adopts to avoid reliance on fossil fuels. |
| `CASUAL_BLOG` | Sarah spent the whole weekend hiking in the mountains with her dog. | `hi` | i hike to avoid altitude sickness. they start by following trails. she wanders to meet local park rangers. it undergoes a minor change to meet outdoor survival experts. he sets out to avoid altitude sickness. he wanders to avoid getting lost. he hikes. |
| `FORMAL_EMAIL` | The team will present the quarterly results to stakeholders on Friday. | `hi` | they are scheduled to avoid inaccuracies in financial reporting. they will report using advanced software tools. he presents with department heads. they lead with key investors. they communicate to avoid misinterpretation. she delivers to avoid data errors. he adopts using advanced software tools. they lead. |

These samples reflect the coherence pass (de-themed prompts + `CLAUSE_END_MAX_PHRASES=2`) plus the verblessness pass: every sentence now opens with a pronoun + finite verb ("he heads to the office ...", "they establish to avoid ..."), so the earlier verbless fragments ("that to office.", "the process has seen with key partners.") are gone. Remaining visible nits — pronoun-referent drift across sentences and similar register across all four styles — are the deferred items above.

## `doublespeak` console app (`imp/doublespeak-console` branch)

A small CLI in `meteor_stego/apps/` (`doublespeak.h`/`.c`/`doublespeak_main.c`) that
drives the library's public `meteor.h` API end-to-end, so encode/decode can be
exercised by hand without writing a throwaway test program. Built as a new
executable target in `meteor_stego/CMakeLists.txt` alongside the existing test
binaries (`add_executable(doublespeak apps/doublespeak.c apps/doublespeak_main.c)`,
linked against the `meteor` library) — the separate `doublespeak/` VS scaffold
folder is unrelated and untouched.

**Usage:** `doublespeak message [-f] [-d] context passphrase [style_index] [outputpath] [URL]`
- `message` — text to encode, or (with `-f`) a filepath to read it from. Under
  `-d` this is the covertext to decode instead.
- `-f` — treat `message` as a filepath.
- `-d` — decode instead of encode (default: encode).
- `context` / `passphrase` — mandatory; the shared starting context and the
  passphrase `meteor_create()` derives the HKDF key from.
- `style_index` / `outputpath` / `URL` — optional and **strictly positional**
  (can't skip one and supply a later one): style `1-4` (default `1`, maps
  directly onto `MeteorStyle`'s `INFORMAL_CHAT..NEWS_ARTICLE`), an output
  filepath (default: stdout), and the llama-server URL (default:
  `http://127.0.0.1:8080`).
- No arguments, or any parse error, prints the usage guide and exits `1`.

**Defaults baked into `doublespeak_run()`:** `beta=3`, `num_candidates=8`,
`max_steps=256`, `llm_timeout_ms=30000` (matching the test suite's own
defaults), and **`salt=NULL` (zero salt)** — the CLI has no salt argument, so
every invocation shares a zero HKDF salt. That's a real reduction in security
for production use, but acceptable for this CLI's stated purpose (exercising
the encode/decode flow, not being a hardened secure-messaging tool).

**Tests:** `meteor_stego/tests/test_doublespeak_cli.c` (target
`test_doublespeak_cli`, ctest name `doublespeak_cli`), two groups in one file —
argument-parsing permutations (fast, no server needed, call
`doublespeak_parse_args()` directly) and a real encode→decode round trip
through `doublespeak_run()` (both direct-text and `-f` file variants, the
latter using the committed `tests/fixtures/sample_message.txt`), gated on
`llama-server` being reachable via the same `server_reachable()`/
`SKIP_REGULAR_EXPRESSION "llama-server not reachable"` convention the other
LLM-backed tests use.

**Live encode progress:** there's no reliable way to predict total step count
or per-step latency ahead of time — in style mode, bits recovered per step
(`step.cp_len` in `meteor_core.c`) depends on the live LLM probability
distribution at that step, not just message size (a 2-byte "hi" message has
taken anywhere from ~8 to 20+ phrase steps across samples captured during
development), and per-step latency depends on model/hardware/context length.
So instead of an upfront ETA, `meteor.h` exposes an additive, non-breaking
`meteor_encode_ex()` (`MeteorProgressFn progress_cb` + `void*
progress_userdata` appended to `meteor_encode()`'s params) that invokes the
callback synchronously, once per step, from inside the existing encode loop —
no new threads. `meteor_encode()` itself is unchanged (a thin wrapper calling
`meteor_encode_ex(..., NULL, NULL, ...)`), and neither FFI binding
(`bindings/python/meteor.py`, `bindings/csharp/Meteor.cs`) needs updating,
since they don't reference the new function and `MeteorConfig`'s layout is
untouched (those bindings were already stale before this change — the Python
one is missing the `style` field — so this was also a deliberate choice not to
add to that drift).

`doublespeak_run()`'s encode path (`apps/doublespeak.c`) uses
`meteor_encode_ex()` to print a rolling progress line **to stderr** (stdout /
`outputpath` stay clean for the actual covertext) after every step: elapsed
time always, plus an estimated remaining time and projected finish-time-of-day
once the step has recovered at least one bit (the estimate refines each step
as more real rate data accumulates; it can appear as early as step 1 if that
step yielded bits). The decode path has no equivalent — decode's message
length isn't known until decoding finishes, and there's no existing per-step
sampling primitive for it the way `meteor_estimate_capacity()` gives encode.

## Dependencies

| Dependency | Windows | Linux/macOS |
|---|---|---|
| libsodium ≥ 1.0.18 | **Vendored** — prebuilt MSVC x64 static libs in `meteor_stego/deps/libsodium/libsodium/x64/{Debug,Release}/v143/static/libsodium.lib` | `apt install libsodium-dev` / homebrew |
| HTTP client | **WinHTTP** (Windows-native, zero install) — enabled via `METEOR_HTTP_WINHTTP` compile def | libcurl: `apt install libcurl4-openssl-dev` |
| cJSON 1.7.x | **Vendored** in `meteor_stego/third_party/cjson/` — avoids `/Za`+`/std:c11` FetchContent clash | Same vendored copy |
| libhyphen ≥ 2.8 | Optional (`METEOR_USE_LIBHYPHEN=ON`); heuristic fallback is the default | `apt install libhyphen-dev` |
| llama.cpp | `ExternalProject_Add`; first build takes 5–15 min | Same |
| hyph_en_US.dic | `meteor_stego/data/` — from LibreOffice dictionaries (Apache-2.0) | Same |

**Why WinHTTP instead of libcurl on Windows:** libcurl MSVC builds aren't available from curl.se (only MinGW), and vcpkg is blocked by SSL on this machine. WinHTTP ships with Windows and requires no installation.

## Determinism constraint

This is the most critical operational requirement. A single-bit difference in LLM probability distributions between encoder and decoder corrupts the entire recovered message. Both sides must use:
- The **same GGUF model file** (verify SHA-256 with `scripts/verify_model.sh`)
- The **same `--threads` count** on encoder and decoder (`METEOR_NUM_THREADS`, default 1 — safest cross-machine). llama.cpp's CPU backend is reproducible for a *fixed* thread count; a mismatch causes float reduction-order divergence. See "Performance tuning" above.
- CPU-only inference (`--gpu-layers 0`); GPU float rounding differs across vendors
- A pinned llama.cpp git tag (`LLAMA_CPP_GIT_TAG` in CMake)
- `"cache_prompt": false` on every `/completion` request body (`llm_client.c`) — llama-server's default `cache_prompt: true` reuses KV cache across unrelated requests sharing the same slot, which can change a request's output depending on the server's prior request history even with identical prompt/seed/temp/threads. Discovered via style-mode `styled_encode` failures that were only reproducible after the server had processed a long unrelated request history (see the style-mode Status note above) — confirmed fixed by disabling cache reuse outright.

## Security design

- **PRNG:** ChaCha20 with a 256-bit key derived via HKDF-SHA256. Do not substitute a non-cryptographic PRNG — a 32-bit state limits effective key space to 2³² regardless of key length.
- **Key input:** `MeteorConfig` accepts either a raw 32-byte key (`key_raw`) or arbitrary caller material (`key_input` + `key_input_len`) that is passed through HKDF internally.
- **Wipe on destroy:** `prng_wipe` uses `sodium_memzero` (not `memset`) to guarantee the key is zeroed even under compiler optimisation. Call it inside `meteor_destroy`.
- **EOW token:** The end-of-word marker is `\x01` internally and `·` (U+00B7, middle dot) in LLM prompts. It is never written to the output covertext.
