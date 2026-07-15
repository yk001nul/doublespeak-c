"""
FastAPI application — the Phase 1 single-worker MVP.

Async job model: encode/decode submit a job and return 202 with a job_id; the
client polls GET /v1/jobs/{id}. This proves the async model end-to-end against a
local llama-server, with an in-memory job store and no GCP dependencies. The
job-document shape and endpoint contract match SERVICE_ARCHITECTURE.md so the
store/queue can later be swapped for Firestore/Cloud Tasks without API changes.
"""
import logging
from contextlib import asynccontextmanager

from fastapi import FastAPI, HTTPException
from fastapi.responses import JSONResponse

from . import config, worker
from .binding import Meteor
from .dispatcher import Dispatcher
from .jobs import JobStore
from .schemas import DecodeRequest, EncodeRequest, JobAccepted, JobStatus, Progress

logging.basicConfig(level=logging.INFO)
log = logging.getLogger("meteor.service")

store = JobStore()
dispatcher = Dispatcher(store)


@asynccontextmanager
async def lifespan(app: FastAPI):
    dispatcher.start()
    log.info("dispatcher started; llm_url=%s threads=%s",
             config.LLM_URL, config.NUM_THREADS)
    yield
    await dispatcher.stop()


app = FastAPI(
    title="doublespeak stego service",
    version="0.1.0",
    summary="Async encode/decode over the Meteor steganographic library.",
    lifespan=lifespan,
)


def _llm_ready() -> bool:
    """Readiness probe: llama-server reachable + model loaded.

    Uses a throwaway context with a dummy key — health does not depend on key
    material. Any failure (server down, DLL missing) reads as not-ready.
    """
    try:
        return Meteor(key_input=b"readyz-probe", llm_url=config.LLM_URL).health()
    except Exception:  # noqa: BLE001
        return False


# ── Health / contract ────────────────────────────────────────────────────────

@app.get("/healthz")
async def healthz():
    """Liveness: the process is up. Always 200 if serving."""
    return {"status": "ok", "queue_depth": dispatcher.depth()}


@app.get("/readyz")
async def readyz():
    """Readiness: gated on llama-server being up with the model loaded."""
    if _llm_ready():
        return {"status": "ready"}
    return JSONResponse(status_code=503, content={"status": "not_ready",
                                                  "reason": "llm_unreachable"})


@app.get("/v1/model-info")
async def model_info():
    """The served determinism contract. A counterparty decoding outside this
    service must match every field here."""
    return config.model_info()


# ── Job submission ───────────────────────────────────────────────────────────

@app.post("/v1/encode", status_code=202, response_model=JobAccepted)
async def submit_encode(req: EncodeRequest):
    job = store.create("encode", config.model_info())
    await dispatcher.submit(job.id, lambda: worker.run_encode(job.id, req, store))
    return JobAccepted(job_id=job.id, status=job.status)


@app.post("/v1/decode", status_code=202, response_model=JobAccepted)
async def submit_decode(req: DecodeRequest):
    job = store.create("decode", config.model_info())
    await dispatcher.submit(job.id, lambda: worker.run_decode(job.id, req, store))
    return JobAccepted(job_id=job.id, status=job.status)


# ── Job status ───────────────────────────────────────────────────────────────

@app.get("/v1/jobs/{job_id}", response_model=JobStatus)
async def get_job(job_id: str):
    job = store.get(job_id)
    if job is None:
        raise HTTPException(status_code=404, detail="job not found")
    p = job.progress
    return JobStatus(
        job_id=job.id,
        kind=job.kind,
        status=job.status,
        progress=Progress(step=p.step, bits_done=p.bits_done, total_bits=p.total_bits,
                          elapsed_s=p.elapsed_s, eta_s=p.eta_s),
        result=job.result,
        error=job.error,
        model_info=job.model_info,
    )
