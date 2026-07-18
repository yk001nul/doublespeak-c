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
