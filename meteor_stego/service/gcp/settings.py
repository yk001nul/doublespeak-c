"""
GCP-mode configuration, all from environment.

These are read by the front-end and worker apps. They intentionally have no
defaults for the identifiers that must be real (project, queue, bucket, worker
URL) — a missing one should fail loudly at startup, not silently point at the
wrong resource.
"""
import os


def _req(name: str) -> str:
    """A required setting: empty string if unset (validated at app startup via
    validate(), so import never fails and unit tests can monkeypatch)."""
    return os.environ.get(name, "")


def _opt(name: str, default: str) -> str:
    return os.environ.get(name, default)


# Identity / location
GCP_PROJECT = _req("GCP_PROJECT")
REGION      = _opt("GCP_REGION", "us-central1")

# Firestore — job source of truth
FIRESTORE_COLLECTION = _opt("FIRESTORE_COLLECTION", "meteor_jobs")

# Cloud Tasks — dispatch queue to the workers
TASKS_QUEUE    = _req("TASKS_QUEUE")
TASKS_LOCATION = _opt("TASKS_LOCATION", REGION)
# The worker's externally reachable base URL that Cloud Tasks pushes to.
WORKER_URL     = _req("WORKER_URL")
# Service account whose OIDC token authenticates the push to the worker. Read by
# BOTH sides: the front-end signs pushes with it (CloudTasksQueue), and the
# worker validates the incoming token's email against it (see REQUIRE_OIDC).
WORKER_OIDC_SA = _opt("WORKER_OIDC_SA", "")

# ── Worker ingress auth ──────────────────────────────────────────────────────
# The worker's /internal/run is reachable over a public LB, so it must reject
# any push that is not a valid Cloud Tasks OIDC token. Off by default so the
# fake-backed unit tests (and any local run) need no tokens; the k8s ConfigMap
# turns it on in the real deployment.
REQUIRE_OIDC = _opt("WORKER_REQUIRE_OIDC", "false").lower() in ("1", "true", "yes")
# Expected `aud` claim of the incoming OIDC token. Cloud Tasks signs the push
# with audience == the front-end's WORKER_URL, so this MUST equal that value
# (e.g. http://<worker-lb-ip>). Set in the worker ConfigMap.
WORKER_OIDC_AUDIENCE = _opt("WORKER_OIDC_AUDIENCE", "")

# ── Public API: authentication, quota, admission control ─────────────────────
# The front-end is the public door to a backend that costs real money per job,
# so REQUIRE_API_KEY defaults to TRUE — forgetting to set it must not be the
# thing that leaves the service open. Turn it off explicitly for local runs.
REQUIRE_API_KEY     = _opt("REQUIRE_API_KEY", "true").lower() in ("1", "true", "yes")
API_KEYS_COLLECTION = _opt("API_KEYS_COLLECTION", "api_keys")
USAGE_COLLECTION    = _opt("USAGE_COLLECTION", "api_key_usage")
# How long a key record is cached in-process. This is also the window in which a
# revoked key keeps working, so keep it short.
API_KEY_CACHE_TTL_S = float(_opt("API_KEY_CACHE_TTL_S", "60"))

# Default limits applied to a key record that does not override them.
FREE_TIER_QUOTA_DAILY    = int(_opt("FREE_TIER_QUOTA_DAILY", "5"))
FREE_TIER_MAX_CONCURRENT = int(_opt("FREE_TIER_MAX_CONCURRENT", "1"))

# Admission control: refuse new work once the backlog reaches this depth. At
# roughly four minutes per job on a single worker, 10 queued jobs is already a
# ~40 minute wait — past that, a 429 is more useful to the caller than a job id.
ADMISSION_MAX_QUEUED     = int(_opt("ADMISSION_MAX_QUEUED", "10"))
ADMISSION_RETRY_AFTER_S  = int(_opt("ADMISSION_RETRY_AFTER_S", "600"))
CONCURRENCY_RETRY_AFTER_S = int(_opt("CONCURRENCY_RETRY_AFTER_S", "300"))
QUOTA_RETRY_AFTER_S      = int(_opt("QUOTA_RETRY_AFTER_S", "3600"))

# GCS — pinned model + large payloads
GCS_BUCKET        = _req("GCS_BUCKET")
GCS_MODEL_OBJECT  = _opt("GCS_MODEL_OBJECT", "models/Phi-3.5-mini-instruct-Q4_K_M.gguf")
# Where the worker writes the downloaded model on its local filesystem.
LOCAL_MODEL_PATH  = _opt("METEOR_GGUF_PATH", "/models/model.gguf")
# Covertext/result larger than this (bytes) is stored in GCS, not inline in the
# Firestore document (Firestore has a 1 MiB document cap).
RESULT_INLINE_MAX_BYTES = int(_opt("RESULT_INLINE_MAX_BYTES", str(256 * 1024)))
GCS_RESULT_PREFIX = _opt("GCS_RESULT_PREFIX", "results")


def validate_frontend() -> list[str]:
    """Return a list of missing-required-setting messages for the front-end."""
    missing = []
    for name, val in (("GCP_PROJECT", GCP_PROJECT), ("TASKS_QUEUE", TASKS_QUEUE),
                      ("WORKER_URL", WORKER_URL), ("GCS_BUCKET", GCS_BUCKET)):
        if not val:
            missing.append(name)
    return missing


def validate_worker() -> list[str]:
    """Return a list of missing-required-setting messages for the worker."""
    missing = []
    for name, val in (("GCP_PROJECT", GCP_PROJECT), ("GCS_BUCKET", GCS_BUCKET)):
        if not val:
            missing.append(name)
    # When OIDC enforcement is on, both the expected audience and the caller SA
    # must be set — otherwise the worker would accept unauthenticated pushes.
    if REQUIRE_OIDC:
        for name, val in (("WORKER_OIDC_AUDIENCE", WORKER_OIDC_AUDIENCE),
                          ("WORKER_OIDC_SA", WORKER_OIDC_SA)):
            if not val:
                missing.append(name)
    return missing
