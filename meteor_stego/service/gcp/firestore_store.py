"""
FirestoreJobStore — the job source of truth in GCP mode.

Implements the same method surface as `jobs.InMemoryJobStore` (see store.py) so
`worker.run_encode/run_decode` and the API layers are unchanged. Each job is one
Firestore document keyed by job_id; the request payload is persisted so the
worker (a separate process from the front-end) can reconstruct the request.

`google.cloud.firestore` is imported lazily so importing this module — e.g. for
type references or unit tests using a fake store — does not require the library.
"""
import time

from ..jobs import (Job, Progress, QUEUED, RUNNING, DONE, FAILED)
from . import settings


class FirestoreJobStore:
    def __init__(self, project: str | None = None, collection: str | None = None):
        from google.cloud import firestore  # lazy
        self._client = firestore.Client(project=project or settings.GCP_PROJECT)
        self._col = self._client.collection(collection or settings.FIRESTORE_COLLECTION)

    def _doc(self, job_id: str):
        return self._col.document(job_id)

    def create(self, kind: str, model_info: dict, payload: dict | None = None) -> Job:
        import uuid
        job = Job(id=uuid.uuid4().hex, kind=kind, status=QUEUED,
                  model_info=model_info, payload=payload)
        # asdict-friendly document; Progress is nested.
        self._doc(job.id).set(job.to_dict())
        return job

    def get(self, job_id: str) -> Job | None:
        snap = self._doc(job_id).get()
        if not snap.exists:
            return None
        return _job_from_dict(snap.to_dict())

    def mark_running(self, job_id: str) -> None:
        self._doc(job_id).update({"status": RUNNING, "started_at": time.time()})

    def mark_queued(self, job_id: str) -> None:
        # Reset to queued so a re-enqueued push re-runs the job (worker shutdown
        # recovery on preemption / scale-down). See jobs.InMemoryJobStore.
        self._doc(job_id).update({"status": QUEUED, "started_at": None})

    def update_progress(self, job_id: str, *, step: int, bits_done: int,
                        total_bits: int, elapsed_s: float, eta_s: float | None) -> None:
        self._doc(job_id).update({
            "progress": {
                "step": step, "bits_done": bits_done, "total_bits": total_bits,
                "elapsed_s": elapsed_s, "eta_s": eta_s,
            }
        })

    def set_done(self, job_id: str, result: dict) -> None:
        # Firestore documents cap at ~1 MiB; a large covertext is offloaded to
        # GCS and replaced with a gs:// URI the client can fetch.
        covertext = result.get("covertext")
        if isinstance(covertext, str) and \
                len(covertext.encode("utf-8")) > settings.RESULT_INLINE_MAX_BYTES:
            from . import gcs  # lazy
            uri = gcs.put_result(job_id, covertext.encode("utf-8"))
            result = {k: v for k, v in result.items() if k != "covertext"}
            result["covertext_gcs_uri"] = uri
        self._doc(job_id).update({
            "status": DONE, "result": result, "finished_at": time.time()})

    def set_failed(self, job_id: str, error: str) -> None:
        self._doc(job_id).update({
            "status": FAILED, "error": error, "finished_at": time.time()})


def _job_from_dict(d: dict) -> Job:
    prog = d.get("progress") or {}
    return Job(
        id=d["id"], kind=d["kind"], status=d["status"],
        model_info=d.get("model_info") or {}, payload=d.get("payload"),
        progress=Progress(**{k: prog.get(k) for k in
                             ("step", "bits_done", "total_bits", "elapsed_s", "eta_s")
                             if k in prog}),
        result=d.get("result"), error=d.get("error"),
        created_at=d.get("created_at") or 0.0,
        started_at=d.get("started_at"), finished_at=d.get("finished_at"),
    )
