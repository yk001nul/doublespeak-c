"""
Phase 2 GCP-mode tests using FAKES — no google-cloud libraries and no
llama-server required. They exercise the front-end submit flow, the worker
ack-fast / single-slot / dedupe behaviour, the request-payload round trip, and
the GCS SHA-256 verification logic.

Run:  py -3 -m pytest meteor_stego/service/tests/test_gcp.py -v
"""
import base64
import threading

import pytest
from fastapi.testclient import TestClient

from meteor_stego.service.jobs import InMemoryJobStore, QUEUED, RUNNING, DONE
from meteor_stego.service.gcp.frontend_app import app as frontend_app
from meteor_stego.service.gcp.worker_app import app as worker_app


def _b64(data: bytes) -> str:
    return base64.b64encode(data).decode("ascii")


def _encode_body(**over):
    body = {
        "message": "hi",
        "starting_context": "John goes to the office using his car every morning.",
        "style": 1,
        "key_material_b64": _b64(b"shared-secret"),
        "salt_b64": _b64(bytes(range(1, 33))),
    }
    body.update(over)
    return body


class FakeQueue:
    """Records enqueued (job_id, kind) instead of hitting Cloud Tasks."""
    def __init__(self):
        self.enqueued = []

    def enqueue(self, job_id, kind):
        self.enqueued.append((job_id, kind))
        return f"projects/fake/tasks/{job_id}"


# ── Front-end ────────────────────────────────────────────────────────────────

@pytest.fixture
def frontend():
    store = InMemoryJobStore()
    queue = FakeQueue()
    frontend_app.state.store = store
    frontend_app.state.queue = queue
    with TestClient(frontend_app) as c:
        yield c, store, queue
    # reset injected state so tests stay independent
    frontend_app.state.store = None
    frontend_app.state.queue = None


def test_frontend_submit_persists_and_enqueues(frontend):
    client, store, queue = frontend
    r = client.post("/v1/encode", json=_encode_body())
    assert r.status_code == 202
    job_id = r.json()["job_id"]
    assert r.json()["status"] == QUEUED

    # Job persisted with the request payload (so the worker can reconstruct it).
    job = store.get(job_id)
    assert job is not None
    assert job.kind == "encode"
    assert job.payload["message"] == "hi"
    assert job.payload["salt_b64"] == _b64(bytes(range(1, 33)))

    # And a task was enqueued for it.
    assert queue.enqueued == [(job_id, "encode")]


def test_frontend_validation_still_enforced(frontend):
    client, _, _ = frontend
    # zero salt must be rejected before any job is created
    r = client.post("/v1/encode", json=_encode_body(salt_b64=_b64(bytes(32))))
    assert r.status_code == 422


def test_frontend_unknown_job_404(frontend):
    client, _, _ = frontend
    assert client.get("/v1/jobs/nope").status_code == 404


def test_frontend_model_info(frontend):
    client, _, _ = frontend
    j = client.get("/v1/model-info").json()
    assert j["sampling"]["cache_prompt"] is False


# ── Worker ───────────────────────────────────────────────────────────────────

@pytest.fixture
def worker():
    store = InMemoryJobStore()
    ran = []
    done = threading.Event()

    def fake_run_job(job_id, kind, payload, st):
        ran.append((job_id, kind, payload))
        st.set_done(job_id, {"covertext": "he goes to the office.", "elapsed_s": 0.01})
        done.set()

    worker_app.state.store = store
    worker_app.state.run_job = fake_run_job
    worker_app.state.slot = threading.Lock()
    with TestClient(worker_app) as c:
        yield c, store, ran, done
    worker_app.state.store = None
    worker_app.state.run_job = None
    worker_app.state.slot = None
    worker_app.state.verify_oidc = None


def _seed_job(store, kind="encode"):
    return store.create(kind, {"num_threads": 4}, payload=_encode_body())


def test_worker_runs_and_acks_fast(worker):
    client, store, ran, done = worker
    job = _seed_job(store)
    r = client.post("/internal/run", json={"job_id": job.id, "kind": "encode"})
    assert r.status_code == 200
    assert r.json()["status"] == "running"
    # Background task completes (TestClient runs it after the response).
    assert done.wait(5)
    assert ran and ran[0][0] == job.id
    assert store.get(job.id).status == DONE
    assert store.get(job.id).result["covertext"]


def test_worker_dedupes_redelivery(worker):
    client, store, ran, done = worker
    job = _seed_job(store)
    store.mark_running(job.id)  # simulate already picked up
    r = client.post("/internal/run", json={"job_id": job.id, "kind": "encode"})
    assert r.status_code == 200
    assert r.json()["status"] == "already_handled"
    assert ran == []  # not run again


def test_worker_missing_fields_400(worker):
    client, *_ = worker
    assert client.post("/internal/run", json={"job_id": "x"}).status_code == 400


def test_worker_unknown_job_404(worker):
    client, *_ = worker
    r = client.post("/internal/run", json={"job_id": "ghost", "kind": "encode"})
    assert r.status_code == 404


def test_worker_single_slot_rejects_when_busy(worker):
    client, store, ran, done = worker
    # Hold the slot to simulate an in-flight job.
    assert worker_app.state.slot.acquire(blocking=False)
    try:
        job = _seed_job(store)
        r = client.post("/internal/run", json={"job_id": job.id, "kind": "encode"})
        assert r.status_code == 429
        assert store.get(job.id).status == QUEUED  # untouched
    finally:
        worker_app.state.slot.release()


# ── Worker OIDC enforcement ──────────────────────────────────────────────────

def test_worker_oidc_disabled_allows_no_token(worker):
    # Default (REQUIRE_OIDC off) — the existing behaviour: no token needed.
    client, store, ran, done = worker
    job = _seed_job(store)
    r = client.post("/internal/run", json={"job_id": job.id, "kind": "encode"})
    assert r.status_code == 200
    assert done.wait(5)


def test_worker_oidc_required_rejects_missing_token(worker, monkeypatch):
    # With enforcement on and no injected verifier, the real verifier runs and
    # rejects a request that carries no bearer token — 401, before any job work
    # (and without importing the google libraries, since the header check is
    # first). ran stays empty.
    from meteor_stego.service.gcp import settings
    monkeypatch.setattr(settings, "REQUIRE_OIDC", True)
    client, store, ran, done = worker
    job = _seed_job(store)
    r = client.post("/internal/run", json={"job_id": job.id, "kind": "encode"})
    assert r.status_code == 401
    assert ran == []
    assert store.get(job.id).status == QUEUED  # untouched


def test_worker_oidc_required_runs_with_valid_token(worker, monkeypatch):
    # Enforcement on, but a fake verifier accepts (stands in for a real Google
    # token) — the job runs. Proves the accept path is wired through.
    from meteor_stego.service.gcp import settings
    monkeypatch.setattr(settings, "REQUIRE_OIDC", True)
    worker_app.state.verify_oidc = lambda request: None  # accept
    client, store, ran, done = worker
    job = _seed_job(store)
    r = client.post("/internal/run", json={"job_id": job.id, "kind": "encode"},
                    headers={"Authorization": "Bearer faketoken"})
    assert r.status_code == 200
    assert done.wait(5)
    assert ran and ran[0][0] == job.id


def test_oidc_helper_rejects_bad_headers():
    from meteor_stego.service.gcp import oidc
    for bad in (None, "", "Token abc", "Bearer", "Bearer   "):
        with pytest.raises(oidc.OIDCError) as ei:
            oidc.verify_bearer_token(bad, expected_audience="http://x", allowed_sa="sa@x")
        assert ei.value.status == 401  # missing/malformed → 401, no google import


# ── GCS SHA verification ─────────────────────────────────────────────────────

def test_gcs_sha_helper(tmp_path):
    from meteor_stego.service.gcp import gcs
    p = tmp_path / "model.gguf"
    p.write_bytes(b"deterministic-bytes")
    import hashlib
    expected = hashlib.sha256(b"deterministic-bytes").hexdigest()
    assert gcs._sha256_file(str(p)) == expected
