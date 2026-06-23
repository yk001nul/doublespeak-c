# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

**doublespeak-c** is a C library implementing a syllable-level variant of the Meteor steganographic system (Kaptchuk et al., CCS 2021). It encodes binary data into LLM-generated covertext and recovers it with a shared symmetric key. The full target architecture is specified in `ARCHITECTURE.md` — that document is authoritative for what to build.

The `doublespeak/` directory currently contains a Visual Studio CMake scaffold (hello-world placeholder). The implementation should follow the directory layout in `ARCHITECTURE.md §3`.

## Build

The project uses CMake with Ninja and MSVC (Windows). CMake presets are defined in `doublespeak/CMakePresets.json`.

```bash
# Configure (debug, x64 — uses the CMakePresets.json preset)
cmake --preset x64-debug -S doublespeak -B doublespeak/out/build/x64-debug

# Build
cmake --build doublespeak/out/build/x64-debug

# Release
cmake --preset x64-release -S doublespeak -B doublespeak/out/build/x64-release
cmake --build doublespeak/out/build/x64-release
```

Once the library and tests are implemented (per `ARCHITECTURE.md §9`), the full build+test flow is:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DLLAMA_CPP_GIT_TAG=b4693 \
      -DMETEOR_LLM_PORT=8080 \
      -DMETEOR_LLM_SEED=42 \
      -DMETEOR_MODEL_PATH=/path/to/model.gguf

cmake --build build

# Run tests (roundtrip/determinism require a running llama-server)
ctest --test-dir build --output-on-failure

# Run a single test
./build/test_prng
```

## Runtime: llama-server

The library communicates with a locally running `llama-server` (llama.cpp) via HTTP on port 8080. Start it before running any encode/decode operation or the roundtrip/determinism tests:

```bash
./build/start_llama_server.sh /path/to/model.gguf   # Linux/macOS
build\start_llama_server.bat  path\to\model.gguf    # Windows

# Verify it's alive
cmake --build build --target server_health
```

Critical server flags for determinism: `--threads 1 --temp 0.0 --seed 42 --no-mmap`. See `ARCHITECTURE.md §6` for the full determinism checklist.

## Architecture

The library is structured around 7 components (see `ARCHITECTURE.md §4` for full specs):

| Component | Files | Role |
|---|---|---|
| PRNG | `src/prng.c/.h` | ChaCha20 CSPRNG keyed via HKDF-SHA256 (libsodium) |
| Bit Packing | `src/bits.c/.h` | Message ↔ bit array, MSB-first, null-terminated |
| Meteor Core | `src/meteor_core.c/.h` | One encode/decode step: slot table + common-prefix recovery |
| Syllabifier | `src/syllabifier.c/.h` | libhyphen wrapper + heuristic fallback |
| LLM Client | `src/llm_client.c/.h` | libcurl HTTP + cJSON parsing; grammar-constrained GBNF |
| Encode | `src/encode.c/.h` | Full encode pipeline |
| Decode | `src/decode.c/.h` | Full decode pipeline |

Public API is in `include/meteor.h`. FFI bindings (Python ctypes, C# P/Invoke) live in `bindings/`.

**Key invariant:** Encoder and decoder must produce byte-identical LLM prompts at every step. The reconstructed `full_text` string must use exactly the same space separators, word boundaries, and `starting_context` prefix on both sides. Any divergence corrupts all subsequent bit recovery with no error signal.

## Dependencies

| Dependency | Install |
|---|---|
| libsodium ≥ 1.0.18 | `apt install libsodium-dev` / vcpkg / homebrew |
| libhyphen ≥ 2.8 | `apt install libhyphen-dev` / vcpkg / homebrew |
| libcurl ≥ 7.68 | `apt install libcurl4-openssl-dev` / vcpkg |
| cJSON 1.7.x | Vendored in `third_party/cjson/` |
| llama.cpp | Fetched by CMake `ExternalProject_Add`; first build takes 5–15 min |
| hyph_en_US.dic | Ship in `data/`; from LibreOffice dictionaries (Apache-2.0) |

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
