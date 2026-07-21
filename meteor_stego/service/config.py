"""
Service configuration and the served determinism contract.

Every value that affects the encode/decode lockstep is surfaced here and, in
turn, published verbatim by ``GET /v1/model-info``. A counterparty decoding
outside this service must match them exactly (see SERVICE_ARCHITECTURE.md
"Determinism as a served contract").
"""
import functools
import hashlib
import os


def _env(name: str, default: str) -> str:
    return os.environ.get(name, default)


# ── Determinism-critical, served-contract values ────────────────────────────
# These are protocol state, not tuning knobs. Changing NUM_THREADS or the model
# bumps the served protocol version and requires re-validating styled_encode /
# roundtrip (see CLAUDE.md "Performance tuning").

LLM_URL      = _env("METEOR_LLM_URL", "http://127.0.0.1:8080")
NUM_THREADS  = int(_env("METEOR_NUM_THREADS", "1"))   # must match llama-server --threads
LLAMA_CPP_TAG = _env("LLAMA_CPP_TAG", "unknown")
GGUF_PATH    = os.environ.get("METEOR_GGUF_PATH")     # optional; enables SHA computation
GGUF_SHA256  = os.environ.get("METEOR_GGUF_SHA256")   # explicit override, else computed

# Fixed sampling flags the worker's llama-server is expected to run with. These
# are informational (the service does not launch llama-server); they document
# the contract the operator must honour on both encode and decode sides.
SAMPLING = {
    "temp": 0.0,
    "seed": 42,
    "no_mmap": True,
    "parallel": 1,
    "no_cont_batching": True,
    "gpu_layers": 0,
    "cache_prompt": False,
}


# ── Library defaults (match the CLI / test-suite defaults) ───────────────────

DEFAULT_BETA           = 3
DEFAULT_NUM_CANDIDATES = 8    # beta=3 → 8 candidates fills all slots exactly
DEFAULT_MAX_STEPS      = 256
DEFAULT_LLM_TIMEOUT_MS = 30000

# Per-request salt is mandatory for a network service (the zero-salt CLI
# shortcut is explicitly rejected — see SERVICE_ARCHITECTURE.md "Security
# notes").
REQUIRED_SALT_LEN = 32


# ── Request ceilings (public-API abuse control) ──────────────────────────────
# The worker is single-slot and a job costs roughly two minutes of C2 CPU per
# message byte, so an unbounded request is a denial of service against the whole
# service, not just the caller. Every one of these is an upper bound on how long
# one request can occupy the only worker. Raise them per-tier later if needed;
# they are read by schemas.py, so a change here is a change to the public API.

MAX_MESSAGE_BYTES  = int(_env("METEOR_MAX_MESSAGE_BYTES", "32"))    # ≈1h of worker time
MAX_STEPS_LIMIT    = int(_env("METEOR_MAX_STEPS_LIMIT", str(DEFAULT_MAX_STEPS)))
MAX_LLM_TIMEOUT_MS = int(_env("METEOR_MAX_LLM_TIMEOUT_MS", "60000"))
MAX_CONTEXT_CHARS  = int(_env("METEOR_MAX_CONTEXT_CHARS", "2000"))
MAX_COVERTEXT_CHARS = int(_env("METEOR_MAX_COVERTEXT_CHARS", "20000"))


@functools.lru_cache(maxsize=1)
def gguf_sha256() -> str:
    """Resolve the model SHA-256 for the served contract.

    Prefers the explicit ``METEOR_GGUF_SHA256`` override; otherwise computes it
    from ``METEOR_GGUF_PATH`` once and caches. Returns ``"unknown"`` if neither
    is available (dev/local runs where determinism auditing is not enforced).
    """
    if GGUF_SHA256:
        return GGUF_SHA256
    if GGUF_PATH and os.path.exists(GGUF_PATH):
        h = hashlib.sha256()
        with open(GGUF_PATH, "rb") as f:
            for chunk in iter(lambda: f.read(1024 * 1024), b""):
                h.update(chunk)
        return h.hexdigest()
    return "unknown"


def model_info() -> dict:
    """The served determinism contract, returned by GET /v1/model-info and
    embedded in every job document."""
    return {
        "gguf_sha256":   gguf_sha256(),
        "llama_cpp_tag": LLAMA_CPP_TAG,
        "num_threads":   NUM_THREADS,
        "llm_url":       LLM_URL,
        "temp":          SAMPLING["temp"],
        "seed":          SAMPLING["seed"],
        "sampling":      SAMPLING,
    }
