# SERVICE_ARCHITECTURE.md

Design sketch for exposing the doublespeak-c stegosystem as a **FastAPI service on
Google Cloud**. This is a planning document — no service code exists yet. It captures the
architecture, the required GCP subservices, and the determinism/security constraints that
are specific to serving a steganographic system over a network.

See `CLAUDE.md` and `ARCHITECTURE.md` for the underlying C library. The determinism
constraint from `CLAUDE.md` ("Determinism constraint" section) is the single most important
input to this design.

## Scope decision

- **Binding + worker/service code → this repo.** The service must be version-locked to the
  exact compiled `meteor.so`/`meteor.dll` it wraps (a mismatched library silently corrupts
  every message). Keeping them in one repo makes that lockstep automatic. The repo is already
  a monorepo (`meteor_stego/`, `doublespeak/`, `bindings/`), so it fits.
- **GCP infrastructure → separate `infra/` dir or its own repo.** Terraform, Cloud Build
  config, and GKE manifests have a different lifecycle and ownership from a portable C
  library, and would bloat this repo. They consume built, version-pinned container images.

## Deployment unit (the constraint that drives everything)

The atomic runnable unit is a **worker**:

```
FastAPI worker process
  └─ meteor.so (via the refreshed Python ctypes binding)
       └─ llama-server (co-located, single decode slot)
            └─ pinned GGUF model
```

`llama-server` is single-client (`--parallel 1`), so **one worker handles exactly one job at
a time**. Concurrency comes from running *more workers*, never more threads per worker or
more clients per `llama-server`. This single fact drives the queue-based design and the
autoscaling model.

## Phase 0 — Binding refresh (prerequisite, in this repo)

`meteor_stego/bindings/python/meteor.py` is stale and blocks everything else. Before any
service code:

- Expose `style` (1–4), `salt`, `beta`, `num_candidates`, `max_steps`, `llm_timeout_ms` as
  real parameters. Currently `style=0` (METEOR_STYLE_NONE) is hardcoded, so none of the four
  style modes are reachable.
- Wire `meteor_encode_ex`'s `MeteorProgressFn` callback through ctypes so the worker can
  stream per-step progress/ETA into the job store.
- Keep `_MeteorConfig` byte-identical to `meteor.h`'s `MeteorConfig` (the existing
  struct-drift out-of-bounds-read comment in the binding already warns about this — `style`
  must stay last and present).
- No threading inside a worker — the meteor context and `llama-server` are both single-client;
  concurrency is achieved by running more workers.

## Phase 1 — Async job API

Encode/decode latency is minutes to tens of minutes (roundtrip ~1332s at threads=4), which
rules out synchronous request/response and does not fit Cloud Run's request model. Everything
is a job:

```
POST /v1/encode    → 202 {job_id}   body: {message | message_b64, starting_context,
                                            style, beta, num_candidates, key_material,
                                            salt, callback_url?}
POST /v1/decode    → 202 {job_id}   body: {covertext, starting_context, style, key_material,
                                            salt}
GET  /v1/jobs/{id} →     {status: queued|running|done|failed,
                          progress: {step, elapsed_s, eta_s},
                          result?, error?,
                          model_info: {gguf_sha256, llama_cpp_tag, num_threads}}
GET  /v1/model-info →    {gguf_sha256, llama_cpp_tag, num_threads, temp, seed, sampling}
GET  /healthz
GET  /readyz            (readiness gated on meteor_llm_health → llama-server up + model loaded)
```

**Idempotency (a free win from determinism):** the same key + message + context + model +
thread count always produces identical covertext. So at-least-once queue delivery and retries
are safe; dedupe on `job_id` against the job store. Decode is likewise deterministic.

## Phase 2 — GCP topology

```
          (TLS)                                 Cloud Tasks
Client ──────────► Cloud Run  ──enqueue──►  (max_concurrent_dispatches
                  (API front)                = worker count)
                      │  ▲                          │ push
                write │  │ read                     ▼
                      ▼  │                    ┌─────────────────────────┐
                   Firestore ◄──progress──── │  GKE worker pool         │
                 (job state,     updates      │  each pod, concurrency=1:│
                  source of truth)            │   • FastAPI worker       │
                      ▲                        │   • meteor.so (binding)  │
                      │ result                 │   • llama-server sidecar │
                      │                        │   • GGUF (from GCS)      │
                 Cloud Storage ◄──────────────│   CPU-only, no GPU       │
                (large payloads,              └─────────────────────────┘
                 GGUF model,                        ▲ pull model + verify SHA
                 uploaded files)                     │
                                              Secret Manager (API keys,
                                               shared stego keys)
```

**End-to-end flow:**

1. Client → Cloud Run API: `POST /v1/encode` with payload + auth (TLS).
2. API validates, writes a `queued` job document to Firestore, enqueues a Cloud Task pointing
   at a worker endpoint.
3. Cloud Tasks dispatches to a worker, respecting `max_concurrent_dispatches` (= worker count).
4. Worker **acks the task immediately** (see "30-minute deadline trap" below), flips the job
   to `running`, and runs the encode in a background task. `meteor_encode_ex`'s progress
   callback updates the Firestore doc (`step`, `elapsed_s`, `eta_s`) each step.
5. On completion the worker writes the result (covertext to Firestore, or to GCS if large)
   and sets status `done`.
6. Client polls `GET /v1/jobs/{id}` (Cloud Run reading Firestore) or receives the
   `callback_url` webhook.

### Required GCP subservices

| Service | Role | Why this one |
|---|---|---|
| **Cloud Run** | Stateless API front-end (submit jobs, read status) | Fast, scale-to-zero, cheap; it only enqueues and reads Firestore — never runs inference |
| **GKE** (or **GCE** managed instance group) | Worker pool running the heavy stack | Needs long-lived pods with a warm, loaded model and concurrency=1; autoscale on queue depth. GKE for fine control; GCE MIG for simpler/coarser scaling |
| **Cloud Tasks** | Dispatch queue with rate limiting | `max_concurrent_dispatches` / `max_dispatches_per_second` map exactly onto "serialize to N single-client workers." Pub/Sub pull is the alternative if you prefer worker-pull autoscaling |
| **Firestore** | Job state + progress (source of truth) | Serverless document model fits job status; idempotent dedupe by `job_id` |
| **Cloud Storage (GCS)** | Pinned GGUF, large covertext outputs, `-f`-style uploaded message files | Workers pull the model and verify SHA-256 on boot; keeps model versioning auditable |
| **Secret Manager** | Service API keys, shared long-lived stego keys | Keeps keys out of images/env; per-request key material stays in-memory only |
| **Artifact Registry** | Container images (bundling exact `meteor.so` + pinned llama.cpp tag) | Determinism lockstep enforced at build time |
| **Cloud Build** | CI/CD → Artifact Registry → deploy | Builds the determinism-critical image reproducibly |
| **Cloud Load Balancing + API Gateway / Cloud Endpoints** | TLS entry, auth, rate limiting in front of Cloud Run | API-key/IAM auth, quota |
| **Cloud Logging / Monitoring / Trace** | Observability + the queue-depth custom metric that drives worker autoscaling | Also track per-step latency and job durations |

## The 30-minute deadline trap

Cloud Tasks HTTP push (and Pub/Sub ack) have delivery deadlines shorter than a long encode.
**Do not** hold the request open for the whole job. Pattern:

- The task delivery only means "job X is ready."
- The worker records `running` in Firestore, **returns 200 immediately**, and runs the actual
  encode in an in-pod background task, updating Firestore as the source of truth.
- Redelivery is deduped by checking the Firestore job status (safe because jobs are
  deterministic/idempotent).

Pub/Sub pull with ack-deadline extension is the alternative, but ack-fast + Firestore-as-truth
is simpler and leans on the idempotency already available for free.

## Determinism as a served contract

This is the part unique to a stego service and easy to get wrong:

- **GPU is off, permanently** (`--gpu-layers 0`). GPU float rounding breaks the encode/decode
  lockstep. Cost is therefore CPU-bound long-running inference — the dominant cost driver.
- **Every worker must be byte-identical:** same GGUF SHA-256, same `METEOR_NUM_THREADS`, same
  pinned llama.cpp tag, same sampling flags (`--temp 0.0 --seed 42 --no-mmap --parallel 1
  --no-cont-batching`), and `"cache_prompt": false` on every request. Enforced by all pods
  sharing one image + one config. Because any worker can pick up any job, a single mismatched
  worker would silently corrupt messages.
- **Publish the contract** via `GET /v1/model-info`. If the counterparty decodes *outside*
  this service, they must match your thread count and model — `METEOR_NUM_THREADS` is shared
  protocol state (see `CLAUDE.md` "Performance tuning"), so it must be discoverable, not
  implicit.
- Raising `threads` for throughput requires re-validating `styled_encode`/`roundtrip` (per
  `CLAUDE.md`) and bumps the served protocol version — it is not a free scaling knob.

## Security notes

- Per-request key material / passphrases travel over TLS only, are never logged, and rely on
  the library's existing `sodium_memzero`-on-destroy wipe. Long-lived shared keys live in
  Secret Manager, not request bodies.
- The **zero-salt shortcut the CLI takes is not acceptable for a network service** — a network
  endpoint is a far larger attack surface than a local CLI. Require a real 32-byte salt per
  identity/session (shared out-of-band) or derive one; do not default to zero salt.

## Cost & scaling notes

- Dominant cost = always-on CPU worker time (no GPU, long jobs). Right-size worker vCPU to the
  validated thread count (threads=4 is validated on Phi-3.5-mini; ~2.6× faster than threads=1).
- Keep workers **warm** (min replicas ≥ 1): `llama-server` reloads a multi-GB GGUF with
  `--no-mmap` on cold start, so scale-to-zero trades cost for a slow first job.
- Autoscale workers on **queue depth** (custom Cloud Monitoring metric), not CPU — a busy
  worker is at 100% CPU by design, so CPU-based autoscaling would never scale out correctly.
- Front-end (Cloud Run) can scale to zero freely; it does no inference.

## Suggested phasing

1. **Binding refresh** (this repo): style + progress callback + salt. Unblocks all else.
2. **Single-worker MVP**: one FastAPI worker + `llama-server` in one container, Firestore for
   jobs, no autoscaling. Proves the async job model end-to-end.
3. **Split tiers**: front-end (Cloud Run) from worker pool (GKE), add Cloud Tasks +
   queue-depth autoscaling, GCS model with SHA verification, Secret Manager.
4. **Harden**: `/v1/model-info` contract, webhooks, auth/quota via API Gateway, observability
   dashboards.
