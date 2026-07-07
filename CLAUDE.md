# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

**doublespeak-c** is a C library implementing a syllable-level variant of the Meteor steganographic system (Kaptchuk et al., CCS 2021). It encodes binary data into LLM-generated covertext and recovers it with a shared symmetric key. The full target architecture is specified in `ARCHITECTURE.md` — that document is authoritative for what to build.

The `doublespeak/` directory currently contains a Visual Studio CMake scaffold (hello-world placeholder). The implementation should follow the directory layout in `ARCHITECTURE.md §3`.

## Projects

| Directory | Purpose |
|---|---|
| `meteor_stego/` | The Meteor stego C library (main implementation) |
| `doublespeak/` | Original VS-generated C++ scaffold (hello-world placeholder, not the library) |

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

Critical server flags for determinism: `--threads 1 --temp 0.0 --seed 42 --no-mmap`. See `ARCHITECTURE.md §6` for the full determinism checklist.

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
2. `meteor_draw_clause_end()` — decides whether the phrase about to be generated ends the current sentence (append a literal `.`, reset `subject_anchor`, start fresh). Forced to continue below `CLAUSE_END_MIN_PHRASES=2` and forced to end at `CLAUSE_END_MAX_PHRASES=6`, but the PRNG bits are always drawn regardless of which bound fires, to keep stream position identical between encode/decode.
3. `meteor_draw_digress_mode()` / `meteor_draw_digression_axis()` / `meteor_draw_digression_variant()` — decide whether the sentence about to be opened digresses onto a secondary entity instead of paraphrasing the topic, which `DigressionAxis` it asks about, and which of `DIGRESS_VARIANT_COUNT` phrasings of that axis's question to use. See "Topic subordination (digression)" below.
4. The beta-bit slot-selection draw inside `meteor_encode_step`/decode's mirror, same as syllable mode.

All of the above draws happen unconditionally every step, including the very first phrase of the covertext (where the question/digression draws are unused) — this is required so PRNG position never depends on data-dependent branches.

**Subject anchor:** the first phrase of each sentence (`subject_anchor[0]=='\0'` on entry) is prompted to open with an explicit subject (pronoun or noun phrase), and that subject is repeated verbatim in every later phrase-dist prompt for the rest of the sentence to keep person/tense consistent. `subject_anchor` (not `ctx_len`/`full_context`) is what `build_phrase_prompt` and `llm_client_get_phrase_dist` key off to decide opening-step vs. continuation-step grammar — `full_context` stays non-empty across sentence boundaries, so it can't be used to detect "first phrase of *this* sentence."

**Anti-repetition:** `MeteorWordHistory` (shared helper in `meteor_core.c/.h`, used identically by encode.c and decode.c so both sides build byte-identical prompts) blacklists the last 8 non-stopword tokens; `PHRASE_HISTORY` blacklists the last 6 whole phrases. Neither resets at sentence boundaries — repeats are suppressed across the entire message, not just within one sentence.

**Decoder note:** the literal `.` inserted at a clause end has no space before it in the covertext and isn't part of any LLM candidate's text, so decode.c must consume it explicitly (`if (*remaining == '.') remaining++`) right after matching the phrase that preceded it, before resuming its normal space-skip.

**Topic subordination (digression):** every sentence used to paraphrase the same fixed topic (`starting_context`, quoted verbatim in `preamble`'s "Original: ..." line for the whole message), which reads as repetitive/implausible for a short context and a long message. `meteor_draw_digress_mode()` (`meteor_core.c`) decides, at each sentence-opening step (`subject_anchor[0]=='\0'`), whether that sentence instead digresses onto a secondary subject/object from the text so far. Digression is gated behind `DIGRESS_MIN_TOPIC_SENTENCES=1` (the opening sentence is always topic-anchored) and capped at one hop via `DigressionState.last_was_digression` (a digression sentence is always followed by a forced topic-anchored sentence) — both checked *after* the PRNG bits are drawn, same forced-bound pattern as `meteor_draw_clause_end`. `meteor_draw_digression_axis()` independently picks one of 5 `DigressionAxis` values (DESCRIBE/STATE/SIGNIFICANCE/ORIGIN/OUTCOME); `meteor_draw_digression_variant()` picks one of `DIGRESS_VARIANT_COUNT` alternate phrasings of that axis's question, purely for wording variety. All three draws happen every step (like `StyleQuestion`) but are only consulted — and only latched into `sentence_is_digression`/`sentence_axis`/`sentence_variant` for later use at the sentence's clause-end — on opening steps.

A digression opening step is a **two-stage** process, not a single LLM call: (1) `llm_client_get_digression_answer()` (`llm_client.c`) asks `DIGRESSION_QUESTION[axis][variant]` — a plain, non-bit-embedding, no-grammar `/completion` call (temp 0, seed 42) about a secondary entity picked by the model from the text so far — and returns its one-sentence answer; this call carries no message-payload bits, it only establishes what to talk about, and both encode.c/decode.c get byte-identical text since it's deterministic (falling back to a fixed local string, `DIGRESSION_FALLBACK_ANSWER`, on HTTP/parse failure so lockstep survives a server hiccup). (2) that answer is fed into `llm_client_build_preamble()` to build a one-off temporary preamble ("Paraphrase the sentence below... Original: `<answer>`") which is passed to `llm_client_get_phrase_dist()` **in place of** the main topic preamble for this one opening-step call — taking the exact same code path (`OPENING_SUBJECT_GRAMMAR`, beta-bit slot selection) as a normal topic-anchored opening, so real message bits still get embedded in whichever paraphrase-of-the-answer is selected. The raw question is never written to the covertext; only the paraphrased answer is. Continuation steps of that sentence are unaffected — still `STYLE_QUESTION_PHASE`/`build_phrase_grammar` against the main preamble, relying on `subj_clause` (naming the digression-derived opener verbatim) to keep them anchored to the digression's subject rather than the main topic.

**Status:** the bare-verb sentence-opener issue (above) was fixed via `OPENING_SUBJECT_GRAMMAR` (commit `0494509`, `imp/topic-subordination`), confirmed with a full `roundtrip`/`determinism`/`capacity` ctest rerun. Topic subordination (digression, above) is newly implemented on `imp/topic-subordination` and not yet verified — needs the same build/spot-check/full-ctest-rerun process before merge. Neither change is merged to `main` yet.

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
- `--threads 1` (eliminates float reduction-order non-determinism)
- CPU-only inference (`--gpu-layers 0`); GPU float rounding differs across vendors
- A pinned llama.cpp git tag (`LLAMA_CPP_GIT_TAG` in CMake)

## Security design

- **PRNG:** ChaCha20 with a 256-bit key derived via HKDF-SHA256. Do not substitute a non-cryptographic PRNG — a 32-bit state limits effective key space to 2³² regardless of key length.
- **Key input:** `MeteorConfig` accepts either a raw 32-byte key (`key_raw`) or arbitrary caller material (`key_input` + `key_input_len`) that is passed through HKDF internally.
- **Wipe on destroy:** `prng_wipe` uses `sodium_memzero` (not `memset`) to guarantee the key is zeroed even under compiler optimisation. Call it inside `meteor_destroy`.
- **EOW token:** The end-of-word marker is `\x01` internally and `·` (U+00B7, middle dot) in LLM prompts. It is never written to the output covertext.
