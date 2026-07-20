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

Because step 3 acks the push before the work is done, Cloud Tasks drops the task
and will NOT retry it. So if the node is preempted (Spot) or drained (scheduled
scale-down) mid-run, the job would be orphaned in `running` forever. On SIGTERM
(graceful pod shutdown) `recover_inflight()` resets the in-flight job to `queued`
and re-enqueues a fresh push so a replacement worker re-runs it — deterministic,
so identical output. Requires uvicorn to run as PID 1 (exec form in
Dockerfile.worker) so the signal reaches this process.

Testability: `store`, `run_job`, and `enqueue` live in `app.state`; tests inject
fakes so the module imports without the binding or google-cloud libraries, and
`recover_inflight()` is a plain function unit-tested directly.
"""
import logging
import signal
import threading
from contextlib import asynccontextmanager

from fastapi import BackgroundTasks, FastAPI, Request
from fastapi.responses import JSONResponse

from .. import config
from ..jobs import QUEUED, RUNNING
from ..schemas import DecodeRequest, EncodeRequest
from . import settings

log = logging.getLogger("meteor.service.worker")


def recover_inflight(store, enqueue, inflight) -> bool:
    """Re-queue the job that was running when the pod is being shut down.

    The worker acks each push fast (returns 200, then runs the job in a
    background task), so once a job is running Cloud Tasks has already dropped
    the task and will NOT retry it. If the node is preempted (Spot) or drained
    (scheduled scale-down) mid-run, the job would otherwise be orphaned in the
    `running` state forever. On SIGTERM we reset it to `queued` and enqueue a
    fresh push so a replacement worker re-runs it — jobs are deterministic, so
    the re-run yields byte-identical output.

    Guarded on the job still being RUNNING, so a job that finished between the
    signal and here is left untouched. Returns True if a job was re-queued.
    """
    if not inflight:
        return False
    job_id, kind = inflight.get("job_id"), inflight.get("kind")
    if not job_id or not kind:
        return False
    job = store.get(job_id)
    if job is None or job.status != RUNNING:
        return False
    store.mark_queued(job_id)
    # dedupe_name=False: the original name=job_id task already executed, so
    # reusing it would be rejected for ~1h (see CloudTasksQueue.enqueue).
    enqueue(job_id, kind, dedupe_name=False)
    log.warning("re-queued in-flight job %s on shutdown", job_id)
    return True


def _install_termination_handler(app) -> None:
    """On SIGTERM (graceful pod shutdown: preemption / scale-down / rollout),
    recover any in-flight job before the process dies, then chain to whatever
    handler was already installed (uvicorn's, which begins graceful shutdown).

    signal.signal only works in the main thread, so under TestClient (lifespan
    runs off-main-thread) this is skipped — recover_inflight() is unit-tested
    directly instead. Requires the container to run uvicorn as PID 1 (exec form
    in Dockerfile.worker) so the signal actually reaches this process."""
    try:
        prev = signal.getsignal(signal.SIGTERM)
    except (ValueError, OSError):
        return

    def _handler(signum, frame):
        try:
            recover_inflight(app.state.store, app.state.enqueue,
                             getattr(app.state, "inflight", None))
        except Exception:  # noqa: BLE001 — never block shutdown on a recovery error
            log.exception("in-flight recovery on SIGTERM failed")
        if callable(prev) and prev not in (signal.SIG_DFL, signal.SIG_IGN):
            prev(signum, frame)

    try:
        signal.signal(signal.SIGTERM, _handler)
    except ValueError:
        log.warning("SIGTERM handler not installed (not main thread); "
                    "in-flight recovery via signal is unavailable")


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
    if not getattr(app.state, "enqueue", None):
        # Re-enqueue path for shutdown recovery. The worker now needs Cloud Tasks
        # enqueuer + act-as-invoker IAM and TASKS_QUEUE/WORKER_URL config (added
        # in Terraform + the ConfigMap).
        from .cloud_tasks import CloudTasksQueue
        _queue = CloudTasksQueue()
        app.state.enqueue = _queue.enqueue
    if not getattr(app.state, "inflight", None):
        app.state.inflight = None
    _install_termination_handler(app)
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
    state = request.app.state

    # Single decode slot: refuse a second concurrent job so Cloud Tasks retries.
    if not slot.acquire(blocking=False):
        return JSONResponse(status_code=429, content={"status": "busy"})

    # The slot is released by _work() on the one path that schedules it; every
    # other exit (404 / already_handled / error) must release it here, or the
    # worker would wedge into permanent 429 after a redelivery.
    scheduled = False
    try:
        job = store.get(job_id)
        if job is None:
            return JSONResponse(status_code=404, content={"error": "job not found"})
        # Dedupe redelivery: anything past queued was already picked up.
        if job.status != QUEUED:
            return {"status": "already_handled", "job_status": job.status}

        store.mark_running(job_id)
        # Record what is running so SIGTERM recovery can re-queue it if the node
        # goes away mid-run. Cleared in _work()'s finally.
        state.inflight = {"job_id": job_id, "kind": kind}

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
                state.inflight = None
                slot.release()

        background.add_task(_work)
        scheduled = True
        return {"status": "running", "job_id": job_id}
    finally:
        if not scheduled:
            slot.release()


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
