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
from meteor_stego.service.gcp.auth import Caller, hash_key
from meteor_stego.service.gcp.frontend_app import app as frontend_app
from meteor_stego.service.gcp.worker_app import app as worker_app, recover_inflight

GOOD_KEY  = "dsk_live_good-test-key"
OTHER_KEY = "dsk_live_other-test-key"
AUTH      = {"Authorization": f"Bearer {GOOD_KEY}"}


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
        self.dedupe_names = []

    def enqueue(self, job_id, kind, *, dedupe_name=True):
        self.enqueued.append((job_id, kind))
        self.dedupe_names.append(dedupe_name)
        return f"projects/fake/tasks/{job_id}"


class FakeApiKeyStore:
    """Two known keys, no Firestore. Limits are per-Caller so a test can tighten
    one key without touching global settings."""
    def __init__(self):
        self.records = {
            hash_key(GOOD_KEY):  Caller(key_hash=hash_key(GOOD_KEY),  label="good"),
            hash_key(OTHER_KEY): Caller(key_hash=hash_key(OTHER_KEY), label="other"),
        }

    def lookup(self, key_hash):
        return self.records.get(key_hash)

    def caller(self, raw_key) -> Caller:
        return self.records[hash_key(raw_key)]


class FakeUsage:
    def __init__(self):
        self.counts = {}

    def increment(self, key_hash):
        self.counts[key_hash] = self.counts.get(key_hash, 0) + 1
        return self.counts[key_hash]


# ── Front-end ────────────────────────────────────────────────────────────────

@pytest.fixture
def frontend():
    store = InMemoryJobStore()
    queue = FakeQueue()
    frontend_app.state.store = store
    frontend_app.state.queue = queue
    frontend_app.state.api_keys = FakeApiKeyStore()
    frontend_app.state.usage = FakeUsage()
    with TestClient(frontend_app, headers=AUTH) as c:
        yield c, store, queue
    # reset injected state so tests stay independent
    frontend_app.state.store = None
    frontend_app.state.queue = None
    frontend_app.state.api_keys = None
    frontend_app.state.usage = None


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
    # Attributed to the submitting key, which is what gates reads on it.
    assert job.owner == hash_key(GOOD_KEY)

    # And a task was enqueued for it.
    assert queue.enqueued == [(job_id, "encode")]


def test_frontend_validation_still_enforced(frontend):
    client, _, _ = frontend
    # zero salt must be rejected before any job is created
    r = client.post("/v1/encode", json=_encode_body(salt_b64=_b64(bytes(32))))
    assert r.status_code == 422


def test_validation_error_does_not_echo_key_material(frontend):
    """A 422 must not reflect the caller's secret back in the response body.

    Both error shapes are checked: a body-level validator (whose `input` is the
    entire request, key material included) and a field-level one.
    """
    client, _, _ = frontend
    secret, salt = _b64(b"shared-secret"), _b64(bytes(range(1, 33)))

    for body in (_encode_body(salt_b64=_b64(bytes(32))),   # model validator
                 _encode_body(max_steps=10**6)):           # field validator
        r = client.post("/v1/encode", json=body)
        assert r.status_code == 422
        assert secret not in r.text and salt not in r.text
        # The reason still has to reach the caller, or the 422 is useless.
        err = r.json()["detail"][0]
        assert err["msg"] and err["loc"]
        assert "input" not in err and "ctx" not in err


def test_frontend_unknown_job_404(frontend):
    client, _, _ = frontend
    assert client.get("/v1/jobs/nope").status_code == 404


def test_frontend_model_info(frontend):
    client, _, _ = frontend
    j = client.get("/v1/model-info").json()
    assert j["sampling"]["cache_prompt"] is False


# ── Front-end auth ───────────────────────────────────────────────────────────

def test_submit_without_key_401(frontend):
    _, _, queue = frontend
    bare = TestClient(frontend_app)          # no default Authorization header
    r = bare.post("/v1/encode", json=_encode_body())
    assert r.status_code == 401
    assert queue.enqueued == []              # nothing reached the queue


def test_submit_with_unknown_key_401(frontend):
    client, _, queue = frontend
    r = client.post("/v1/encode", json=_encode_body(),
                    headers={"Authorization": "Bearer dsk_live_nonexistent"})
    assert r.status_code == 401
    assert queue.enqueued == []


def test_disabled_key_401(frontend):
    client, _, _ = frontend
    frontend_app.state.api_keys.caller(GOOD_KEY).disabled = True
    assert client.post("/v1/encode", json=_encode_body()).status_code == 401


def test_x_api_key_header_accepted(frontend):
    _, _, _ = frontend
    bare = TestClient(frontend_app)
    r = bare.post("/v1/encode", json=_encode_body(),
                  headers={"X-API-Key": GOOD_KEY})
    assert r.status_code == 202


def test_other_callers_job_is_404_not_403(frontend):
    client, store, _ = frontend
    job_id = client.post("/v1/encode", json=_encode_body()).json()["job_id"]
    # The owner can read it...
    assert client.get(f"/v1/jobs/{job_id}").status_code == 200
    # ...but a different valid key must not learn that it exists at all.
    r = client.get(f"/v1/jobs/{job_id}",
                   headers={"Authorization": f"Bearer {OTHER_KEY}"})
    assert r.status_code == 404


# ── Front-end quota + admission control ──────────────────────────────────────

def test_daily_quota_exhausted_429(frontend):
    client, _, _ = frontend
    caller = frontend_app.state.api_keys.caller(GOOD_KEY)
    caller.quota_daily = 2
    caller.max_concurrent = 0        # disable the concurrency gate for this test

    assert client.post("/v1/encode", json=_encode_body()).status_code == 202
    assert client.post("/v1/encode", json=_encode_body()).status_code == 202
    r = client.post("/v1/encode", json=_encode_body())
    assert r.status_code == 429
    assert r.headers["Retry-After"]


def test_concurrency_limit_429(frontend):
    client, _, _ = frontend
    # Free-tier default is one in-flight job; the first stays queued.
    assert client.post("/v1/encode", json=_encode_body()).status_code == 202
    r = client.post("/v1/encode", json=_encode_body())
    assert r.status_code == 429
    assert "in flight" in r.json()["detail"]


def test_admission_control_rejects_when_backlog_deep(frontend, monkeypatch):
    from meteor_stego.service.gcp import settings as gcp_settings
    client, store, queue = frontend
    monkeypatch.setattr(gcp_settings, "ADMISSION_MAX_QUEUED", 1)
    frontend_app.state.api_keys.caller(GOOD_KEY).max_concurrent = 0

    assert client.post("/v1/encode", json=_encode_body()).status_code == 202
    r = client.post("/v1/encode", json=_encode_body())
    assert r.status_code == 429
    assert "capacity" in r.json()["detail"]
    assert len(queue.enqueued) == 1


def test_admission_rejection_does_not_burn_daily_quota(frontend, monkeypatch):
    from meteor_stego.service.gcp import settings as gcp_settings
    client, _, _ = frontend
    monkeypatch.setattr(gcp_settings, "ADMISSION_MAX_QUEUED", 0)

    assert client.post("/v1/encode", json=_encode_body()).status_code == 429
    # The daily counter is the only gate that mutates, and it runs last.
    assert frontend_app.state.usage.counts == {}


# ── Worker ───────────────────────────────────────────────────────────────────

@pytest.fixture
def worker():
    store = InMemoryJobStore()
    queue = FakeQueue()
    ran = []
    done = threading.Event()

    def fake_run_job(job_id, kind, payload, st):
        ran.append((job_id, kind, payload))
        st.set_done(job_id, {"covertext": "he goes to the office.", "elapsed_s": 0.01})
        done.set()

    worker_app.state.store = store
    worker_app.state.run_job = fake_run_job
    worker_app.state.slot = threading.Lock()
    worker_app.state.enqueue = queue.enqueue  # shutdown-recovery re-enqueue path
    with TestClient(worker_app) as c:
        yield c, store, ran, done
    worker_app.state.store = None
    worker_app.state.run_job = None
    worker_app.state.slot = None
    worker_app.state.verify_oidc = None
    worker_app.state.enqueue = None
    worker_app.state.inflight = None


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


def test_worker_releases_slot_on_already_handled(worker):
    # Regression: an early-return path (redelivery / 404) must release the slot,
    # or the worker wedges into permanent 429. A redelivered (already running)
    # job must NOT leave the slot held.
    client, store, ran, done = worker
    handled = _seed_job(store)
    store.mark_running(handled.id)
    r = client.post("/internal/run", json={"job_id": handled.id, "kind": "encode"})
    assert r.json()["status"] == "already_handled"
    # Slot must be free: a fresh queued job still runs.
    assert not worker_app.state.slot.locked()
    fresh = _seed_job(store)
    r2 = client.post("/internal/run", json={"job_id": fresh.id, "kind": "encode"})
    assert r2.status_code == 200 and r2.json()["status"] == "running"
    assert done.wait(5)


def test_worker_releases_slot_on_404(worker):
    client, store, ran, done = worker
    r = client.post("/internal/run", json={"job_id": "ghost", "kind": "encode"})
    assert r.status_code == 404
    assert not worker_app.state.slot.locked()  # slot released, not leaked


def test_worker_clears_inflight_after_completion(worker):
    client, store, ran, done = worker
    job = _seed_job(store)
    client.post("/internal/run", json={"job_id": job.id, "kind": "encode"})
    assert done.wait(5)
    assert worker_app.state.inflight is None  # cleared in _work() finally


# ── Shutdown recovery (SIGTERM re-enqueue) ───────────────────────────────────

def test_recover_inflight_requeues_running_job():
    store = InMemoryJobStore()
    queue = FakeQueue()
    job = store.create("encode", {"num_threads": 4}, payload=_encode_body())
    store.mark_running(job.id)
    inflight = {"job_id": job.id, "kind": "encode"}

    assert recover_inflight(store, queue.enqueue, inflight) is True
    # Reset to queued so the re-pushed task actually re-runs it...
    assert store.get(job.id).status == QUEUED
    # ...and re-enqueued WITHOUT the job_id dedupe name (original task already ran).
    assert queue.enqueued == [(job.id, "encode")]
    assert queue.dedupe_names == [False]


def test_recover_inflight_skips_finished_job():
    store = InMemoryJobStore()
    queue = FakeQueue()
    job = store.create("encode", {"num_threads": 4}, payload=_encode_body())
    store.set_done(job.id, {"covertext": "x"})  # finished before the signal
    inflight = {"job_id": job.id, "kind": "encode"}

    assert recover_inflight(store, queue.enqueue, inflight) is False
    assert store.get(job.id).status == DONE  # left untouched
    assert queue.enqueued == []


def test_recover_inflight_noop_when_idle():
    store = InMemoryJobStore()
    queue = FakeQueue()
    assert recover_inflight(store, queue.enqueue, None) is False
    assert queue.enqueued == []


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
