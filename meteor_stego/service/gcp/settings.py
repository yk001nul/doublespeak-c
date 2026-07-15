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
# Service account whose OIDC token authenticates the push to the worker.
WORKER_OIDC_SA = _opt("WORKER_OIDC_SA", "")

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
    return missing
