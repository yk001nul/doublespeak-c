# BINDING_REFRESH_TASKS.md

Phase 0 of the FastAPI-on-GCP service plan (see `SERVICE_ARCHITECTURE.md`). This brings
`meteor_stego/bindings/python/meteor.py` current with the C API in
`meteor_stego/include/meteor.h`, so a FastAPI worker can reach every capability the service
needs. **This is the prerequisite that unblocks all service work.**

All line references are to the files as they stand today.

## Why this first

The service wraps the compiled `meteor.so`/`meteor.dll` through this ctypes binding — it does
**not** reimplement any stego logic (that would fork the determinism-critical code). Anything
the binding cannot express, the service cannot offer. Right now the binding cannot select a
style, report progress, or estimate capacity — three things the service design depends on.

## Gap analysis (meteor.h → current binding)

| Capability (meteor.h) | Binding status today | Needed for |
|---|---|---|
| `style` (MeteorStyle 1–4), `meteor.h:25-31,59` | **Hardcoded `style=0`** (`meteor.py:118`); no `__init__` param | Selecting any of the 4 style modes over the API |
| `max_steps`, `llm_timeout_ms`, `meteor.h:56-57` | **Hardcoded** 256 / 30000 (`meteor.py:116-117`) | Per-request tuning / capacity limits |
| `key_raw` (raw 32-byte key), `meteor.h:46` | **Not exposed** — only `key_input` path (`meteor.py:107-109`) | Callers who already have a derived key (ECDH) |
| `meteor_encode_ex` + `MeteorProgressFn`, `meteor.h:98-111` | **Not bound at all** | Per-step progress/ETA streamed into the job store |
| `meteor_estimate_capacity` + `MeteorCapacityEstimate`, `meteor.h:149-178` | **Not bound at all** | Pre-flight "message too long?" check before enqueuing a doomed multi-minute job |
| Error codes `METEOR_ERR_*`, `meteor.h:182-190` | **Not mapped** — raises generic `RuntimeError(f"... {err.value}")` (`meteor.py:132,146`) | Meaningful HTTP status/error bodies |
| `beta`, `num_candidates`, `llm_url`, `hyphen_dict`, `salt`, `key_input` | Exposed ✓ (`meteor.py:99-105`) | — |
| `meteor_create/destroy/encode/decode/free/llm_health/syllabify_word` | Bound ✓ (`meteor.py:58-83`) | — |

## Task list

### T1 — Expose `style` (unblocks all style modes)
- Add a `MeteorStyle` `IntEnum` mirroring `meteor.h:25-31` (`NONE=0, INFORMAL_CHAT=1,
  FORMAL_EMAIL=2, CASUAL_BLOG=3, NEWS_ARTICLE=4`).
- Add `style: MeteorStyle = MeteorStyle.NONE` to `Meteor.__init__` and pass it into
  `_MeteorConfig` in place of the hardcoded `0` at `meteor.py:118`.
- **Acceptance:** a round trip with `style=MeteorStyle.FORMAL_EMAIL` produces style-mode
  covertext (topic not echoed verbatim) and decodes back to the original message.

### T2 — Expose `max_steps` and `llm_timeout_ms`
- Add both as `__init__` params (defaults 256 / 30000, matching current hardcodes at
  `meteor.py:116-117`) and thread them into `_MeteorConfig`.
- **Acceptance:** a tiny `max_steps` on an over-long message surfaces `METEOR_ERR_CAPACITY`
  (see T6), not a silent hang.

### T3 — Expose the raw-key path (`key_raw`)
- Accept an optional `key_raw: bytes` (must be exactly 32 bytes) as an alternative to
  `key_input`. Enforce "exactly one of `key_raw` / `key_input`" and validate `len==32`.
- Set `key_raw` in `_MeteorConfig` and leave `key_input/key_input_len` NULL/0 (per the
  Option-A/Option-B contract in `meteor.h:37-45`).
- **Acceptance:** encode with a 32-byte `key_raw` and decode with the same key round-trips;
  supplying both keys or a wrong-length raw key raises `ValueError` before calling into C.

### T4 — Bind `meteor_encode_ex` + progress callback (the service's main need)
- Define the callback prototype:
  `_ProgressFn = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
  ctypes.c_int)` matching `MeteorProgressFn` (`meteor.h:98`).
- Register `_lib.meteor_encode_ex` restype/argtypes:
  `[c_void_p, c_char_p, c_size_t, c_char_p, _ProgressFn, c_void_p, POINTER(c_int)]`.
- Add `Meteor.encode(..., progress=None)` where `progress` is an optional
  `Callable[[int, int, int], None]` (step, bits_done, total_bits). Wrap it in a `_ProgressFn`
  and pass `NULL` when absent (so plain `encode` still maps to `meteor_encode_ex(..., NULL,
  NULL, ...)`).
- **CTYPES LANDMINE:** the `CFUNCTYPE` instance **must be retained** on the Python side for the
  whole call (assign to a local/`self` attribute). If it is garbage-collected mid-encode, the C
  side calls a freed trampoline and the worker segfaults. Add a comment at the binding site.
- Keep the existing `encode()` signature working (progress defaults to `None`).
- **Acceptance:** encoding a short message invokes the callback ≥1 time with monotonically
  non-decreasing `step`; the returned covertext is byte-identical to a `progress=None` encode
  of the same inputs (callback must not perturb determinism).

### T5 — Bind `meteor_estimate_capacity` (+ `MeteorCapacityEstimate`)
- Add a `_MeteorCapacityEstimate` `ctypes.Structure` matching `meteor.h:149-155`
  (`estimated_bits:int, estimated_bytes:int, estimated_words:int, avg_bits_per_word:float,
  sample_steps_used:int`).
- Register `_lib.meteor_estimate_capacity` → `restype c_int`, argtypes
  `[c_void_p, c_char_p, c_int, POINTER(_MeteorCapacityEstimate)]`.
- Add `Meteor.estimate_capacity(context: str, sample_steps: int = 0) -> dict` returning the
  struct fields; raise on non-`METEOR_OK` return.
- **Acceptance:** `estimate_capacity(topic, 0)` (heuristic, no server) returns `estimated_bytes
  > 0` in microseconds; the service can compare it to `len(message)` before enqueuing.

### T6 — Map error codes to named exceptions
- Add a `METEOR_ERR` code→name table from `meteor.h:182-190` and a small exception hierarchy
  (e.g. `MeteorError(RuntimeError)` with subclasses / a `.code` attribute:
  `Config, Llm, Capacity, Decode, Dict, Oom, Timeout, Crypto`).
- Replace the generic raises at `meteor.py:131-132` and `146` with the mapped exception, so
  the FastAPI layer can translate `METEOR_ERR_CAPACITY` → 422, `METEOR_ERR_LLM/TIMEOUT` → 503,
  etc.
- **Acceptance:** forcing each failure surfaces the correctly-typed exception carrying the
  numeric `.code`.

### T7 — Struct-lockstep guard
- The determinism note at `meteor.py:50-56` already warns `style` must stay last. After T1–T5,
  re-verify `_MeteorConfig` field order/types byte-match `MeteorConfig` (`meteor.h:35-60`)
  exactly — a mismatch is a silent OOB read in `meteor_create`, not a clean error.
- Consider a tiny self-check: assert `ctypes.sizeof(_MeteorConfig)` against a value the C side
  can report (future `meteor_config_size()` helper — note as a follow-up, not required now).
- **Acceptance:** documented manual field-by-field diff vs. `meteor.h` recorded in the PR.

## Testing

- **Fast, no server:** struct sizes/field order (T7), arg validation for T3 (wrong key length,
  both-keys), error-code mapping table (T6), `estimate_capacity(sample_steps=0)` heuristic (T5).
- **Server-backed (gated like the C tests on llama-server reachability):** a style-mode round
  trip per style (T1), a progress-callback encode asserting callback invocation + determinism
  vs. no-callback (T4). Reuse the same Phi-3.5-mini `--threads` setup the ctest suite uses;
  keep `METEOR_NUM_THREADS` consistent.
- Add a `bindings/python/test_meteor.py` (pytest) so this doesn't rely on the C ctest harness.

## Out of scope for Phase 0

- The C# binding (`meteor_stego/bindings/csharp/Meteor.cs`) — same drift, but the service is
  Python; refresh it separately only if a C# consumer appears.
- Any FastAPI / GCP code — that is Phase 1+ in `SERVICE_ARCHITECTURE.md`.
- Packaging the binding as a pip-installable wheel — nice-to-have, not a blocker.

## Suggested branch

`imp/binding-refresh`, off `main` (independent of `imp/modality-fix`, which is awaiting the
PR #16 merge). Binding-only change; no C library or determinism-protocol surface is touched.
