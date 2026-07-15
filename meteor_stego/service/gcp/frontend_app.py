"""
Cloud Run front-end (GCP mode).

Does no inference: it validates a request, writes a `queued` job to Firestore
(persisting the request payload so the worker can reconstruct it), enqueues a
Cloud Task, and reads Firestore for status. Scales to zero.

Testability: `store` and `queue` live in `app.state`. The lifespan creates the
real Firestore/Cloud Tasks clients only if they were not already injected, so
unit tests can set `app.state.store` / `app.state.queue` to fakes and drive the
routes with TestClient without any google-cloud library installed.
"""
import logging
from contextlib import asynccontextmanager

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import JSONResponse

from .. import config
from ..schemas import DecodeRequest, EncodeRequest, JobAccepted, JobStatus, Progress
from . import settings

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
    yield


app = FastAPI(title="doublespeak stego front-end", version="0.2.0", lifespan=lifespan)


async def _submit(request: Request, kind: str, req) -> JobAccepted:
    store = request.app.state.store
    queue = request.app.state.queue
    job = store.create(kind, config.model_info(), payload=req.model_dump(mode="json"))
    queue.enqueue(job.id, kind)
    return JobAccepted(job_id=job.id, status=job.status)


@app.post("/v1/encode", status_code=202, response_model=JobAccepted)
async def submit_encode(req: EncodeRequest, request: Request):
    return await _submit(request, "encode", req)


@app.post("/v1/decode", status_code=202, response_model=JobAccepted)
async def submit_decode(req: DecodeRequest, request: Request):
    return await _submit(request, "decode", req)


@app.get("/v1/jobs/{job_id}", response_model=JobStatus)
async def get_job(job_id: str, request: Request):
    job = request.app.state.store.get(job_id)
    if job is None:
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
