"""
GCS helpers: pull-and-verify the pinned model on worker boot, and store/read
large covertext results.

The model download is determinism-critical: the worker MUST refuse to serve if
the downloaded GGUF's SHA-256 does not match the served contract
(`config.gguf_sha256()` / `METEOR_GGUF_SHA256`). A silently-wrong model corrupts
every message with no error signal.

`google.cloud.storage` is imported lazily.
"""
import hashlib
import os

from . import settings
from .. import config


def _sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def ensure_model(bucket: str | None = None, obj: str | None = None,
                 dest: str | None = None, expected_sha256: str | None = None) -> str:
    """Download the pinned model from GCS to `dest` (skipping if a byte-for-byte
    match already exists) and verify its SHA-256. Returns the local path.

    Raises RuntimeError on SHA mismatch — the worker must not come up with a
    wrong model.
    """
    from google.cloud import storage  # lazy

    bucket = bucket or settings.GCS_BUCKET
    obj = obj or settings.GCS_MODEL_OBJECT
    dest = dest or settings.LOCAL_MODEL_PATH
    expected = expected_sha256 if expected_sha256 is not None else config.gguf_sha256()

    os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)

    if os.path.exists(dest) and expected not in ("", "unknown"):
        if _sha256_file(dest) == expected:
            return dest  # already have the right file

    client = storage.Client(project=settings.GCP_PROJECT)
    client.bucket(bucket).blob(obj).download_to_filename(dest)

    if expected not in ("", "unknown"):
        actual = _sha256_file(dest)
        if actual != expected:
            raise RuntimeError(
                f"model SHA-256 mismatch: expected {expected}, got {actual}. "
                "Refusing to serve — determinism would be broken.")
    return dest


def put_result(job_id: str, data: bytes, bucket: str | None = None,
               prefix: str | None = None) -> str:
    """Upload a large result to GCS and return its gs:// URI."""
    from google.cloud import storage  # lazy
    bucket = bucket or settings.GCS_BUCKET
    prefix = prefix if prefix is not None else settings.GCS_RESULT_PREFIX
    name = f"{prefix}/{job_id}"
    storage.Client(project=settings.GCP_PROJECT).bucket(bucket).blob(name)\
        .upload_from_string(data)
    return f"gs://{bucket}/{name}"


def get_result(gcs_uri: str) -> bytes:
    """Download a result previously stored via put_result."""
    from google.cloud import storage  # lazy
    assert gcs_uri.startswith("gs://"), gcs_uri
    bucket, _, name = gcs_uri[len("gs://"):].partition("/")
    return storage.Client(project=settings.GCP_PROJECT).bucket(bucket).blob(name)\
        .download_as_bytes()
