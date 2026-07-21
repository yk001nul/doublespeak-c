# doublespeak stego service — Phase 1 MVP

A FastAPI wrapper around the Meteor steganographic library. This is the
**single-worker MVP** described in `SERVICE_ARCHITECTURE.md` ("Suggested phasing"
step 2): it proves the async job model end-to-end against a local `llama-server`,
with an **in-memory job store** and **no GCP dependencies**. The endpoint
contract and job-document shape match the full design, so the store and queue
can later be swapped for Firestore / Cloud Tasks without changing the API.

## Why every request is a job

Encode/decode latency is minutes to tens of minutes (dominated by CPU-only LLM
inference), which rules out synchronous request/response. Submitting an encode or
decode returns `202 {job_id}` immediately; the client polls
`GET /v1/jobs/{id}` for progress and the result.

## Concurrency model (important)

The co-located `llama-server` is single-client (`--parallel 1`), so **exactly one
job runs at a time**. A single-consumer dispatcher backed by a one-thread executor
(`dispatcher.py`) enforces this. Throughput comes from running **more worker
processes**, never from widening the executor or the server's client count.

## Endpoints

| Method & path | Purpose |
|---|---|
| `POST /v1/encode` | Submit an encode job → `202 {job_id, status}` |
| `POST /v1/decode` | Submit a decode job → `202 {job_id, status}` |
| `GET /v1/jobs/{id}` | Job status, per-step progress (`step`, `bits_done`, `elapsed_s`, `eta_s`), result/error, and the model contract the job ran under |
| `GET /v1/model-info` | The served determinism contract (GGUF SHA-256, llama.cpp tag, thread count, sampling flags) |
| `GET /healthz` | Liveness (always 200 while serving) + queue depth |
| `GET /readyz` | Readiness — 200 only when `llama-server` is reachable with the model loaded, else 503 |

### Request fields

Message and key material cross the wire base64-encoded. **A 32-byte non-zero
`salt_b64` is required** — the zero-salt shortcut the CLI takes is rejected for a
network service (`SERVICE_ARCHITECTURE.md` "Security notes").

```jsonc
// POST /v1/encode
{
  "message": "hi",                    // or "message_b64": "<base64 bytes>" (exactly one)
  "starting_context": "John goes to the office using his car every morning.",
  "style": 1,                         // 1=chat 2=email 3=blog 4=news; 0=legacy
  "key_material_b64": "<base64>",     // HKDF input; or "key_raw_b64" for a 32-byte key (exactly one)
  "salt_b64": "<base64 of 32 bytes>", // REQUIRED, non-zero
  "beta": 3, "num_candidates": 8      // optional; default to the library/CLI defaults
}
```

## Determinism as a served contract

`GET /v1/model-info` publishes the exact configuration a counterparty must match
to decode outside this service: `gguf_sha256`, `llama_cpp_tag`, `num_threads`,
and the sampling flags (`temp=0.0`, `seed=42`, `cache_prompt=false`,
`parallel=1`, `no_cont_batching`, `gpu_layers=0`). `METEOR_NUM_THREADS` is shared
protocol state — encoder and decoder must use the identical value.

## Configuration (environment variables)

| Var | Default | Meaning |
|---|---|---|
| `METEOR_LLM_URL` | `http://127.0.0.1:8080` | llama-server URL |
| `METEOR_NUM_THREADS` | `1` | Served protocol state; **must match the running `llama-server --threads`** |
| `LLAMA_CPP_TAG` | `unknown` | Pinned llama.cpp tag, for the model-info contract |
| `METEOR_GGUF_PATH` | — | If set, its SHA-256 is computed for the contract |
| `METEOR_GGUF_SHA256` | — | Explicit SHA-256 override (skips computation) |

## Running locally

1. Build `meteor.dll` (see repo `CLAUDE.md`) and start `llama-server` with the
   deterministic flags (`start_llama_server.bat models\Phi-3.5-mini-instruct-Q4_K_M.gguf`,
   `METEOR_NUM_THREADS=4` if using 4 threads).
2. Install service deps: `py -3 -m pip install -r meteor_stego/service/requirements.txt`
3. Launch the API (from the repo root so the package resolves):
   ```
   set METEOR_NUM_THREADS=4
   py -3 -m uvicorn meteor_stego.service.app:app --host 127.0.0.1 --port 8000
   ```
4. Interactive docs at `http://127.0.0.1:8000/docs`.

## Tests

```
py -3 -m pytest meteor_stego/service/tests/test_service.py -v
```

Fast tests (validation, salt enforcement, 404s, model-info, job lifecycle) always
run. The end-to-end encode→decode round trip and the progress-advances test are
skipped unless a `llama-server` is reachable.

## Request ceilings

Every request is bounded, because one encode costs roughly two minutes of
exclusive worker CPU per message byte — an unbounded request is a denial of
service against the whole service, not just its caller. The limits live in
`config.py` and are enforced by `schemas.py`:

| Field | Default limit | Env override |
|---|---|---|
| `message` / `message_b64` | 32 bytes | `METEOR_MAX_MESSAGE_BYTES` |
| `max_steps` | 256 | `METEOR_MAX_STEPS_LIMIT` |
| `llm_timeout_ms` | 60000 | `METEOR_MAX_LLM_TIMEOUT_MS` |
| `starting_context` | 2000 chars | `METEOR_MAX_CONTEXT_CHARS` |
| `covertext` | 20000 chars | `METEOR_MAX_COVERTEXT_CHARS` |

## Auth and quota (GCP mode only)

The local app in `app.py` has no authentication — it is a single-process dev
mode. The Cloud Run front-end (`gcp/frontend_app.py`) requires an API key on
every route and is the deployment that faces the internet:

- **Keys** (`gcp/auth.py`) are bearer credentials sent as `Authorization: Bearer
  dsk_live_…` or `X-API-Key`. Only the SHA-256 of a key is stored, as its
  Firestore document id. `REQUIRE_API_KEY` defaults to **true** — forgetting an
  env var must not be what leaves the service open. Mint keys with
  `python -m meteor_stego.service.gcp.manage_keys create --label …`.
- **Jobs are owned.** `GET /v1/jobs/{id}` returns 404, not 403, for a job
  belonging to a different key, so it cannot be used to probe which ids exist.
- **Three quota gates** (`gcp/quota.py`) run per submit, cheapest first and
  reads before writes, so a request rejected for capacity does not burn the
  caller's daily allowance: global backlog admission control, per-key
  concurrency, then per-key daily quota. All answer 429 with `Retry-After`.
- The caller's key material is dropped from the job document once the job
  reaches a terminal status.

## Not in this phase

Autoscaling, Cloud Tasks, Firestore, and the Cloud Run split are Phase 2+
(`SERVICE_ARCHITECTURE.md`). This MVP keeps everything in one process to
validate the async model first. Webhooks (`callback_url`) and self-serve key
signup are still outstanding.
