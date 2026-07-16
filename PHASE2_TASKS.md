# PHASE2_TASKS.md — GCP split (apply-later IaC)

Phase 2 of the FastAPI-on-GCP plan (`SERVICE_ARCHITECTURE.md` "Suggested phasing"
step 3 / "Phase 2 — GCP topology"). Splits the Phase 1 single-process MVP
(`meteor_stego/service/`, PR #18) into a **Cloud Run front-end** + a **GKE worker
pool** connected by **Cloud Tasks** with **Firestore** as the job source of
truth and **GCS** for the pinned model and large payloads.

**Decisions (2026-07-16):** apply-later IaC (no live deploy this session); GKE
for workers; infra in an `infra/` dir in this repo.

The Phase 1 local single-process app (`app.py`, in-memory store, in-process
dispatcher) is **kept working unchanged** as the local/dev mode. Phase 2 adds
GCP-mode components beside it; they share `schemas.py`, `worker.py` (the actual
encode/decode calls), `config.py`, and `binding.py`.

## Constraint that shapes the split

- Front-end does **no inference** — it validates, writes a `queued` job to
  Firestore (with the request payload persisted so the worker can reconstruct
  it), enqueues a Cloud Task, and reads Firestore for status. Scales to zero.
- Worker owns the heavy stack (meteor.so + llama-server + GGUF), concurrency=1.
  A Cloud Tasks push means "job X is ready": the worker **acks immediately
  (200)**, flips the job to `running`, and runs the encode/decode in a
  background task, updating Firestore each step. Redelivery is deduped on job
  status (safe — jobs are deterministic/idempotent). This is the "30-minute
  deadline trap" mitigation.

## Code tasks (this repo, `meteor_stego/service/gcp/`)

All GCP client libraries are **lazy-imported** (inside functions/constructors)
so the local MVP and the fake-backed unit tests need none of them installed.

- **P2-1 `gcp/settings.py`** — GCP config from env: `GCP_PROJECT`, region,
  `FIRESTORE_COLLECTION`, `TASKS_QUEUE` + location, `WORKER_URL`, `WORKER_OIDC_SA`
  (audience for the push OIDC token), `GCS_BUCKET`, `GCS_MODEL_OBJECT`,
  `RESULT_INLINE_MAX_BYTES` (covertext larger than this goes to GCS).
- **P2-2 `store.py`** — extract the job-store method surface into a `JobStore`
  Protocol; keep `InMemoryJobStore` (moved from `jobs.py`, re-exported for
  back-compat) and add persistence of the request payload to the job document.
- **P2-3 `gcp/firestore_store.py`** — `FirestoreJobStore` implementing the same
  surface against a Firestore collection. Stores payload + status + progress +
  result/result_gcs_uri + model_info. Lazy `google.cloud.firestore`.
- **P2-4 `gcp/cloud_tasks.py`** — `CloudTasksQueue.enqueue(job_id, kind)` creates
  an HTTP push task to `WORKER_URL/internal/run` carrying `{job_id, kind}`, with
  an OIDC token for authenticated invocation. Lazy `google.cloud.tasks_v2`.
- **P2-5 `gcp/gcs.py`** — `ensure_model()` downloads `GCS_MODEL_OBJECT` on worker
  boot and verifies SHA-256 against the served contract (aborts on mismatch —
  determinism); `put_result`/`get_result` for large covertext. Lazy
  `google.cloud.storage`.
- **P2-6 `gcp/frontend_app.py`** — Cloud Run app: `POST /v1/encode|decode`
  (validate → write `queued` job with payload → enqueue Cloud Task → 202),
  `GET /v1/jobs/{id}`, `GET /v1/model-info`, `/healthz`, `/readyz`. No binding
  load (front-end never touches meteor.so).
- **P2-7 `gcp/worker_app.py`** — GKE worker app: `POST /internal/run` (Cloud
  Tasks push target) — dedupe by status, mark running, **return 200 now**, run
  the job in `BackgroundTasks` via `worker.run_encode/run_decode`, persist result
  (Firestore inline or GCS). `/healthz`, `/readyz` (gated on
  `meteor_llm_health`). `ensure_model()` on startup.
- **P2-8 tests `tests/test_gcp.py`** — fakes for store/queue/gcs (no google libs
  needed): front-end submit writes+enqueues; worker ack-fast + dedupe + result
  persistence; GCS SHA verify on a temp file. Keep Phase 1 tests green.
- **P2-9 `requirements-gcp.txt`** — pinned google-cloud-firestore / -tasks /
  -storage, separate from the base MVP requirements.

## Infra tasks (`infra/`, Terraform + Cloud Build + GKE manifests)

- **P2-10 `infra/terraform/`** — Firestore (native mode), Cloud Tasks queue
  (`max_concurrent_dispatches` = worker count, `max_dispatches_per_second`), GCS
  bucket (model + payloads), Artifact Registry repo, Secret Manager, Cloud Run
  (front-end) service + IAM, GKE cluster + CPU node pool sized to the validated
  thread count, service accounts + least-privilege IAM bindings, the queue-depth
  custom metric. `variables.tf` / `outputs.tf` / `versions.tf`.
- **P2-11 `infra/docker/`** — `Dockerfile.frontend` (slim: FastAPI + google libs,
  no binding) and `Dockerfile.worker` (multi-stage: build meteor.so via the
  linux-release preset + llama.cpp at the pinned tag, then a runtime stage with
  llama-server + the binding; GGUF pulled at boot from GCS, not baked in).
- **P2-12 `infra/cloudbuild/cloudbuild.yaml`** — build+push both images to
  Artifact Registry, tagged with the git SHA (determinism-critical image
  identity).
- **P2-13 `infra/k8s/`** — worker `Deployment` (min replicas ≥ 1 to stay warm),
  `Service`, `HorizontalPodAutoscaler` on the queue-depth external/custom metric
  (NOT CPU — a busy worker is 100% CPU by design), determinism `ConfigMap`
  (`METEOR_NUM_THREADS`, model object, llama.cpp tag, sampling).
- **P2-14 `infra/README.md`** — apply order, prerequisites, and the
  determinism-as-served-contract checklist for operators.

## Out of scope for Phase 2 (→ Phase 4 "Harden")

Auth/quota via API Gateway, webhooks (`callback_url`), observability dashboards,
per-identity Secret Manager stego keys. Live `terraform apply` (deferred by the
apply-later decision).
