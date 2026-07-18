"""
GKE worker (GCP mode).

Owns the heavy stack (meteor.so + llama-server + GGUF), concurrency=1. A Cloud
Tasks push to `POST /internal/run` means "job X is ready". The worker:

  1. rejects the push with 429 if it is already busy (single decode slot) so
     Cloud Tasks retries with backoff — this, plus the queue's
     max_concurrent_dispatches, keeps exactly one job per worker;
  2. dedupes: a job not in `queued` state is treated as already handled
     (redelivery) and acked 200 (safe — jobs are deterministic/idempotent);
  3. flips the job to `running`, **returns 200 immediately**, and runs the
     encode/decode in a background task, updating Firestore each step — the
     "30-minute deadline trap" mitigation.

Testability: `store` and `run_job` live in `app.state`; tests inject fakes so
the module imports without the binding or google-cloud libraries.
"""
import logging
import threading
from contextlib import asynccontextmanager

from fastapi import BackgroundTasks, FastAPI, Request
from fastapi.responses import JSONResponse

from .. import config
from ..jobs import QUEUED
from ..schemas import DecodeRequest, EncodeRequest
from . import settings

log = logging.getLogger("meteor.service.worker")


def _default_run_job(job_id: str, kind: str, payload: dict, store) -> None:
    """Reconstruct the request from the persisted payload and run it. Imports
    the binding-backed worker lazily so this module imports without meteor.so."""
    from .. import worker  # lazy (loads meteor.dll on import)
    if kind == "encode":
        worker.run_encode(job_id, EncodeRequest(**payload), store)
    elif kind == "decode":
        worker.run_decode(job_id, DecodeRequest(**payload), store)
    else:
        raise ValueError(f"unknown job kind: {kind}")


def _default_verify_oidc(request: Request):
    """Validate the incoming Cloud Tasks OIDC token, or return a rejection
    response. Returns None when the token is accepted. Imports gcp.oidc lazily so
    the fake-backed tests that never enable REQUIRE_OIDC stay google-free."""
    from . import oidc  # lazy
    try:
        oidc.verify_bearer_token(
            request.headers.get("authorization"),
            expected_audience=settings.WORKER_OIDC_AUDIENCE,
            allowed_sa=settings.WORKER_OIDC_SA)
    except oidc.OIDCError as exc:
        log.warning("rejected /internal/run push: %s", exc)
        return JSONResponse(status_code=exc.status, content={"error": str(exc)})
    return None


@asynccontextmanager
async def lifespan(app: FastAPI):
    if not getattr(app.state, "store", None):
        missing = settings.validate_worker()
        if missing:
            raise RuntimeError(f"missing required settings: {', '.join(missing)}")
        from .firestore_store import FirestoreJobStore
        from . import gcs
        gcs.ensure_model()  # download + SHA-verify the pinned model, or abort
        app.state.store = FirestoreJobStore()
    if not getattr(app.state, "run_job", None):
        app.state.run_job = _default_run_job
    if not getattr(app.state, "slot", None):
        app.state.slot = threading.Lock()
    if not getattr(app.state, "verify_oidc", None):
        app.state.verify_oidc = _default_verify_oidc
    yield


app = FastAPI(title="doublespeak stego worker", version="0.2.0", lifespan=lifespan)


@app.post("/internal/run")
async def run(request: Request, background: BackgroundTasks):
    # The endpoint is publicly reachable (Cloud Tasks only pushes to public
    # URLs), so authenticate the caller before anything else. Skipped only when
    # REQUIRE_OIDC is off (local/dev + fake-backed tests).
    if settings.REQUIRE_OIDC:
        rejection = request.app.state.verify_oidc(request)
        if rejection is not None:
            return rejection

    body = await request.json()
    job_id, kind = body.get("job_id"), body.get("kind")
    if not job_id or not kind:
        return JSONResponse(status_code=400, content={"error": "job_id and kind required"})

    store = request.app.state.store
    slot: threading.Lock = request.app.state.slot

    # Single decode slot: refuse a second concurrent job so Cloud Tasks retries.
    if not slot.acquire(blocking=False):
        return JSONResponse(status_code=429, content={"status": "busy"})

    try:
        job = store.get(job_id)
        if job is None:
            return JSONResponse(status_code=404, content={"error": "job not found"})
        # Dedupe redelivery: anything past queued was already picked up.
        if job.status != QUEUED:
            return {"status": "already_handled", "job_status": job.status}

        store.mark_running(job_id)
    except Exception:
        slot.release()
        raise

    run_job = request.app.state.run_job

    def _work():
        try:
            run_job(job_id, kind, job.payload, store)
        except Exception as exc:  # noqa: BLE001
            log.exception("job %s failed", job_id)
            try:
                store.set_failed(job_id, f"{type(exc).__name__}: {exc}")
            except Exception:
                log.exception("could not mark job %s failed", job_id)
        finally:
            slot.release()

    background.add_task(_work)
    return {"status": "running", "job_id": job_id}


@app.get("/healthz")
async def healthz(request: Request):
    busy = getattr(request.app.state, "slot", None)
    return {"status": "ok", "busy": bool(busy and busy.locked())}


@app.get("/readyz")
async def readyz(request: Request):
    # Ready only when llama-server is reachable with the model loaded.
    try:
        from ..binding import Meteor
        ok = Meteor(key_input=b"readyz-probe", llm_url=config.LLM_URL).health()
    except Exception:  # noqa: BLE001
        ok = False
    if ok:
        return {"status": "ready"}
    return JSONResponse(status_code=503, content={"status": "not_ready",
                                                  "reason": "llm_unreachable"})
