"""
In-memory, thread-safe job store (Phase 1 MVP).

This stands in for Firestore in the full GCP design. It is deliberately simple:
a dict guarded by a lock, with the same document shape a Firestore-backed store
would expose (status, progress, result, error, model_info), so the API layer
does not change when the backing store is swapped in Phase 3.

Thread-safety matters because the encode progress callback fires from the
worker thread while the API coroutine may read the same job concurrently.
"""
import threading
import time
import uuid
from dataclasses import dataclass, field, asdict

# Job lifecycle: queued → running → (done | failed)
QUEUED  = "queued"
RUNNING = "running"
DONE    = "done"
FAILED  = "failed"


@dataclass
class Progress:
    step: int = 0
    bits_done: int = 0
    total_bits: int = 0
    elapsed_s: float = 0.0
    eta_s: float | None = None


@dataclass
class Job:
    id: str
    kind: str                      # "encode" | "decode"
    status: str
    model_info: dict
    progress: Progress = field(default_factory=Progress)
    result: dict | None = None
    error: str | None = None
    created_at: float = field(default_factory=time.time)
    started_at: float | None = None
    finished_at: float | None = None

    def to_dict(self) -> dict:
        d = asdict(self)
        return d


class JobStore:
    def __init__(self):
        self._jobs: dict[str, Job] = {}
        self._lock = threading.Lock()

    def create(self, kind: str, model_info: dict) -> Job:
        job = Job(id=uuid.uuid4().hex, kind=kind, status=QUEUED, model_info=model_info)
        with self._lock:
            self._jobs[job.id] = job
        return job

    def get(self, job_id: str) -> Job | None:
        with self._lock:
            return self._jobs.get(job_id)

    def mark_running(self, job_id: str) -> None:
        with self._lock:
            job = self._jobs[job_id]
            job.status = RUNNING
            job.started_at = time.time()

    def update_progress(self, job_id: str, *, step: int, bits_done: int,
                        total_bits: int, elapsed_s: float, eta_s: float | None) -> None:
        with self._lock:
            p = self._jobs[job_id].progress
            p.step = step
            p.bits_done = bits_done
            p.total_bits = total_bits
            p.elapsed_s = elapsed_s
            p.eta_s = eta_s

    def set_done(self, job_id: str, result: dict) -> None:
        with self._lock:
            job = self._jobs[job_id]
            job.status = DONE
            job.result = result
            job.finished_at = time.time()

    def set_failed(self, job_id: str, error: str) -> None:
        with self._lock:
            job = self._jobs[job_id]
            job.status = FAILED
            job.error = error
            job.finished_at = time.time()
