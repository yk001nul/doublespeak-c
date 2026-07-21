"""
The JobStore contract, shared by the local in-memory store and the GCP
Firestore store.

Both `jobs.InMemoryJobStore` and `gcp.firestore_store.FirestoreJobStore`
implement this surface, so `worker.run_encode/run_decode` and the API layers are
agnostic to which backing store is in use. Swapping stores is a wiring change,
not an API change.
"""
from typing import Protocol, runtime_checkable

from .jobs import Job, InMemoryJobStore  # noqa: F401  (re-exported for callers)


@runtime_checkable
class JobStore(Protocol):
    def create(self, kind: str, model_info: dict, payload: dict | None = None,
               owner: str | None = None) -> Job:
        """Create a queued job (optionally persisting the request payload and the
        SHA-256 hex of the API key that owns it) and return it."""
        ...

    def get(self, job_id: str) -> Job | None:
        ...

    def count_active_for_owner(self, owner: str) -> int:
        """Jobs this owner currently has queued or running (per-caller
        concurrency gate; see gcp.quota)."""
        ...

    def count_queued(self) -> int:
        """Global queued-job count, for admission control."""
        ...

    def mark_running(self, job_id: str) -> None:
        ...

    def mark_queued(self, job_id: str) -> None:
        """Reset a running job back to queued (worker shutdown recovery)."""
        ...

    def update_progress(self, job_id: str, *, step: int, bits_done: int,
                        total_bits: int, elapsed_s: float, eta_s: float | None) -> None:
        ...

    def set_done(self, job_id: str, result: dict) -> None:
        ...

    def set_failed(self, job_id: str, error: str) -> None:
        ...
