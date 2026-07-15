"""
The blocking encode/decode workers.

These functions call the C library through the ctypes binding and BLOCK for the
whole job (minutes to tens of minutes). They are always run inside a
single-thread executor by the queue consumer (see queue.py) so that at most one
job touches the single-client llama-server at a time — concurrency is achieved
by running more worker *processes*, never more threads here (see
SERVICE_ARCHITECTURE.md "Deployment unit").

The encode path wires meteor_encode_ex's per-step progress callback into the
job store and derives a rolling ETA from the observed bits/second rate, mirror-
ing the doublespeak CLI's stderr progress line.
"""
import time

from . import binding, config
from .jobs import JobStore


def _build_meteor(req, style: int):
    """Construct a Meteor context from a request's shared secret + params."""
    return binding.Meteor(
        key_input      = req.key_material(),
        key_raw        = req.key_raw(),
        salt           = req.salt(),
        beta           = req.beta,
        num_candidates = req.num_candidates,
        llm_url        = config.LLM_URL,
        max_steps      = req.max_steps,
        llm_timeout_ms = req.llm_timeout_ms,
        style          = binding.MeteorStyle(style),
    )


def run_encode(job_id: str, req, store: JobStore) -> None:
    """Encode `req.message_bytes()` into covertext, streaming progress."""
    m = _build_meteor(req, req.style)
    start = time.monotonic()

    def on_step(step: int, bits_done: int, total_bits: int) -> None:
        elapsed = time.monotonic() - start
        # ETA only once at least one bit has landed (rate is meaningless before).
        eta = None
        if bits_done > 0 and total_bits > bits_done:
            rate = bits_done / elapsed          # bits per second
            if rate > 0:
                eta = (total_bits - bits_done) / rate
        store.update_progress(job_id, step=step, bits_done=bits_done,
                              total_bits=total_bits, elapsed_s=elapsed, eta_s=eta)

    covertext = m.encode(req.message_bytes(), req.starting_context, progress=on_step)
    store.set_done(job_id, {
        "covertext": covertext,
        "elapsed_s": time.monotonic() - start,
    })


def run_decode(job_id: str, req, store: JobStore) -> None:
    """Recover message bytes from covertext (no per-step progress available —
    decode length is unknown until it finishes)."""
    m = _build_meteor(req, req.style)
    start = time.monotonic()
    raw = m.decode(req.covertext, req.starting_context)

    result = {"message_b64": _b64(raw), "elapsed_s": time.monotonic() - start}
    # Offer a decoded string too when the payload is valid UTF-8.
    try:
        result["message"] = raw.decode("utf-8")
    except UnicodeDecodeError:
        result["message"] = None
    store.set_done(job_id, result)


def _b64(data: bytes) -> str:
    import base64
    return base64.b64encode(data).decode("ascii")
