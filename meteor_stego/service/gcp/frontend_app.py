"""
Cloud Run front-end (GCP mode).

Does no inference: it validates a request, writes a `queued` job to Firestore
(persisting the request payload so the worker can reconstruct it), enqueues a
Cloud Task, and reads Firestore for status. Scales to zero.

Testability: `store`, `queue`, `api_keys` and `usage` live in `app.state`. The
lifespan creates the real Firestore/Cloud Tasks clients only if they were not
already injected, so unit tests can set them to fakes and drive the routes with
TestClient without any google-cloud library installed.

Authentication is per-API-key (`auth.require_api_key`) and every submit passes
through `quota.enforce`. A job is readable only by the key that created it.
"""
import logging
from contextlib import asynccontextmanager

from fastapi import Depends, FastAPI, HTTPException, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse

from .. import config
from ..schemas import DecodeRequest, EncodeRequest, JobAccepted, JobStatus, Progress
from . import quota, settings
from .auth import Caller, require_api_key

log = logging.getLogger("meteor.service.frontend")


@asynccontextmanager
async def lifespan(app: FastAPI):
    if not getattr(app.state, "store", None):
        missing = settings.validate_frontend()
        if missing:
            raise RuntimeError(f"missing required settings: {', '.join(missing)}")
        from .firestore_store import FirestoreJobStore
        from .cloud_tasks import CloudTasksQueue
        app.state.store = FirestoreJobStore()
        app.state.queue = CloudTasksQueue()
        log.info("front-end up; project=%s queue=%s worker=%s",
                 settings.GCP_PROJECT, settings.TASKS_QUEUE, settings.WORKER_URL)
    if settings.REQUIRE_API_KEY and not getattr(app.state, "api_keys", None):
        from .auth import FirestoreApiKeyStore
        app.state.api_keys = FirestoreApiKeyStore()
        app.state.usage = quota.FirestoreUsageCounter()
        log.info("api key auth ON; quota=%d/day concurrency=%d admission=%d queued",
                 settings.FREE_TIER_QUOTA_DAILY, settings.FREE_TIER_MAX_CONCURRENT,
                 settings.ADMISSION_MAX_QUEUED)
    yield


app = FastAPI(title="doublespeak stego front-end", version="0.2.0", lifespan=lifespan)


@app.exception_handler(RequestValidationError)
async def validation_error(request: Request, exc: RequestValidationError):
    """Say what was wrong without echoing the request back.

    FastAPI's default handler reports the offending value in `input`, and for a
    body-level validator that value is the *whole* body — which here carries
    `key_material_b64` and `salt_b64`, the caller's shared secret. Validation
    errors are the responses most likely to be pasted into a bug report or a
    chat log, so the payload is dropped entirely and only the location and
    reason are returned. `ctx` goes with it: for model-level validators it
    holds the original exception object.

    This mirrors, on the request path, the same rule the storage path already
    follows by dropping key material from a job at terminal status.
    """
    detail = [{"type": e.get("type"), "loc": e.get("loc"), "msg": e.get("msg")}
              for e in exc.errors()]
    return JSONResponse(status_code=422, content={"detail": detail})


async def _submit(request: Request, kind: str, req, caller: Caller) -> JobAccepted:
    store = request.app.state.store
    queue = request.app.state.queue
    quota.enforce(caller, store, request.app.state.usage)
    job = store.create(kind, config.model_info(),
                       payload=req.model_dump(mode="json"), owner=caller.key_hash)
    queue.enqueue(job.id, kind)
    log.info("accepted %s job=%s caller=%s", kind, job.id, caller.short)
    return JobAccepted(job_id=job.id, status=job.status)


@app.post("/v1/encode", status_code=202, response_model=JobAccepted)
async def submit_encode(req: EncodeRequest, request: Request,
                        caller: Caller = Depends(require_api_key)):
    return await _submit(request, "encode", req, caller)


@app.post("/v1/decode", status_code=202, response_model=JobAccepted)
async def submit_decode(req: DecodeRequest, request: Request,
                        caller: Caller = Depends(require_api_key)):
    return await _submit(request, "decode", req, caller)


@app.get("/v1/jobs/{job_id}", response_model=JobStatus)
async def get_job(job_id: str, request: Request,
                  caller: Caller = Depends(require_api_key)):
    job = request.app.state.store.get(job_id)
    # Someone else's job is reported as absent, not forbidden — a 403 would
    # confirm the job id exists, which is itself worth hiding.
    if job is None or (job.owner is not None and job.owner != caller.key_hash):
        raise HTTPException(status_code=404, detail="job not found")
    p = job.progress
    return JobStatus(
        job_id=job.id, kind=job.kind, status=job.status,
        progress=Progress(step=p.step, bits_done=p.bits_done, total_bits=p.total_bits,
                          elapsed_s=p.elapsed_s, eta_s=p.eta_s),
        result=job.result, error=job.error, model_info=job.model_info,
    )


@app.get("/v1/model-info")
async def model_info():
    return config.model_info()


@app.get("/healthz")
async def healthz():
    return {"status": "ok"}


@app.get("/readyz")
async def readyz(request: Request):
    # Front-end readiness = its backing store is wired. It does no inference, so
    # it does not gate on llama-server (that is the worker's concern).
    if getattr(request.app.state, "store", None) is not None:
        return {"status": "ready"}
    return JSONResponse(status_code=503, content={"status": "not_ready"})
