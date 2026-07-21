"""
Tests for the Phase 1 MVP service.

Two groups, matching the binding's own convention:
  * Fast, no server — request validation, salt enforcement, 404s, model-info,
    the async job lifecycle up to (but not through) real inference. Always run.
  * Server-backed — a full encode → poll → decode round trip through the live
    API. Skipped unless a llama-server is reachable.

Run:  py -3 -m pytest meteor_stego/service/tests/test_service.py -v
"""
import base64
import os
import time

import pytest
from fastapi.testclient import TestClient

from meteor_stego.service import config
from meteor_stego.service.app import app
from meteor_stego.service.binding import Meteor

LLM_URL = os.environ.get("METEOR_LLM_URL", "http://127.0.0.1:8080")


def _server_reachable() -> bool:
    try:
        return Meteor(key_input=b"probe", llm_url=LLM_URL).health()
    except Exception:
        return False


needs_server = pytest.mark.skipif(
    not _server_reachable(), reason="llama-server not reachable")


@pytest.fixture
def client():
    # TestClient runs the lifespan, so the dispatcher consumer starts.
    with TestClient(app) as c:
        yield c


def _b64(data: bytes) -> str:
    return base64.b64encode(data).decode("ascii")


def _valid_encode_body(**over):
    body = {
        "message": "hi",
        "starting_context": "John goes to the office using his car every morning.",
        "style": 1,
        "key_material_b64": _b64(b"shared-secret"),
        "salt_b64": _b64(bytes(range(32))),  # 32 non-zero bytes
    }
    body.update(over)
    return body


# ── Fast, no-server ─────────────────────────────────────────────────────────

def test_healthz(client):
    r = client.get("/healthz")
    assert r.status_code == 200
    assert r.json()["status"] == "ok"


def test_model_info_contract(client):
    r = client.get("/v1/model-info")
    assert r.status_code == 200
    j = r.json()
    for key in ("gguf_sha256", "llama_cpp_tag", "num_threads", "sampling"):
        assert key in j
    assert j["num_threads"] == config.NUM_THREADS
    # Determinism-critical sampling must be published.
    assert j["sampling"]["temp"] == 0.0
    assert j["sampling"]["cache_prompt"] is False


def test_unknown_job_404(client):
    r = client.get("/v1/jobs/does-not-exist")
    assert r.status_code == 404


def test_salt_required(client):
    body = _valid_encode_body()
    del body["salt_b64"]
    r = client.post("/v1/encode", json=body)
    assert r.status_code == 422


def test_zero_salt_rejected(client):
    r = client.post("/v1/encode", json=_valid_encode_body(salt_b64=_b64(bytes(32))))
    assert r.status_code == 422
    assert "zero" in r.text.lower()


def test_wrong_salt_length_rejected(client):
    r = client.post("/v1/encode", json=_valid_encode_body(salt_b64=_b64(b"tooshort")))
    assert r.status_code == 422


def test_message_xor_message_b64(client):
    # Supplying both is invalid.
    r = client.post("/v1/encode",
                    json=_valid_encode_body(message="hi", message_b64=_b64(b"hi")))
    assert r.status_code == 422
    # Supplying neither is invalid.
    body = _valid_encode_body()
    del body["message"]
    r = client.post("/v1/encode", json=body)
    assert r.status_code == 422


def test_key_material_xor_key_raw(client):
    r = client.post("/v1/encode",
                    json=_valid_encode_body(key_raw_b64=_b64(bytes(range(32)))))
    # both key_material and key_raw present → invalid
    assert r.status_code == 422


def test_key_raw_wrong_length(client):
    body = _valid_encode_body(key_raw_b64=_b64(b"short"))
    del body["key_material_b64"]
    r = client.post("/v1/encode", json=body)
    assert r.status_code == 422


def test_style_out_of_range(client):
    r = client.post("/v1/encode", json=_valid_encode_body(style=9))
    assert r.status_code == 422


# ── Request ceilings (public-API abuse control) ──────────────────────────────
# Each of these bounds how long one request can occupy the single worker.

def test_oversized_message_rejected(client):
    big = "x" * (config.MAX_MESSAGE_BYTES + 1)
    assert client.post("/v1/encode",
                       json=_valid_encode_body(message=big)).status_code == 422


def test_oversized_message_b64_rejected(client):
    body = _valid_encode_body(
        message_b64=_b64(b"x" * (config.MAX_MESSAGE_BYTES + 1)))
    del body["message"]
    assert client.post("/v1/encode", json=body).status_code == 422


def test_message_at_limit_accepted(client):
    body = _valid_encode_body(message="x" * config.MAX_MESSAGE_BYTES)
    assert client.post("/v1/encode", json=body).status_code == 202


def test_max_steps_above_ceiling_rejected(client):
    r = client.post("/v1/encode",
                    json=_valid_encode_body(max_steps=config.MAX_STEPS_LIMIT + 1))
    assert r.status_code == 422


def test_llm_timeout_above_ceiling_rejected(client):
    r = client.post("/v1/encode",
                    json=_valid_encode_body(
                        llm_timeout_ms=config.MAX_LLM_TIMEOUT_MS + 1))
    assert r.status_code == 422


def test_oversized_starting_context_rejected(client):
    r = client.post("/v1/encode",
                    json=_valid_encode_body(
                        starting_context="c" * (config.MAX_CONTEXT_CHARS + 1)))
    assert r.status_code == 422


def test_oversized_covertext_rejected(client):
    body = _valid_encode_body()
    del body["message"]
    body["covertext"] = "c" * (config.MAX_COVERTEXT_CHARS + 1)
    assert client.post("/v1/decode", json=body).status_code == 422


def test_encode_accepts_and_queues(client):
    """Submission returns 202 + job_id and the job is retrievable. Without a
    server the job will eventually fail at inference, but acceptance + the job
    document lifecycle are exercised here regardless."""
    r = client.post("/v1/encode", json=_valid_encode_body())
    assert r.status_code == 202
    job_id = r.json()["job_id"]
    assert r.json()["status"] == "queued"

    r2 = client.get(f"/v1/jobs/{job_id}")
    assert r2.status_code == 200
    j = r2.json()
    assert j["kind"] == "encode"
    assert j["status"] in ("queued", "running", "done", "failed")
    assert "gguf_sha256" in j["model_info"]


# ── Server-backed end-to-end ─────────────────────────────────────────────────

def _poll(client, job_id, timeout_s=1800, interval=2.0):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        j = client.get(f"/v1/jobs/{job_id}").json()
        if j["status"] in ("done", "failed"):
            return j
        time.sleep(interval)
    raise AssertionError(f"job {job_id} did not finish within {timeout_s}s")


@needs_server
def test_encode_decode_roundtrip(client):
    message = "hi"
    ctx = "John goes to the office using his car every morning."
    salt = _b64(bytes(range(1, 33)))
    key = _b64(b"e2e-shared-secret")

    r = client.post("/v1/encode", json={
        "message": message, "starting_context": ctx, "style": 1,
        "key_material_b64": key, "salt_b64": salt,
        "beta": 3, "num_candidates": 8,
    })
    assert r.status_code == 202
    enc = _poll(client, r.json()["job_id"])
    assert enc["status"] == "done", enc.get("error")
    covertext = enc["result"]["covertext"]
    assert covertext

    r = client.post("/v1/decode", json={
        "covertext": covertext, "starting_context": ctx, "style": 1,
        "key_material_b64": key, "salt_b64": salt,
        "beta": 3, "num_candidates": 8,
    })
    assert r.status_code == 202
    dec = _poll(client, r.json()["job_id"])
    assert dec["status"] == "done", dec.get("error")
    assert dec["result"]["message"] == message


@needs_server
def test_progress_advances(client):
    """The encode job reports increasing step counts via the progress callback."""
    r = client.post("/v1/encode", json=_valid_encode_body())
    job_id = r.json()["job_id"]
    seen_running = False
    max_step = 0
    deadline = time.time() + 1800
    while time.time() < deadline:
        j = client.get(f"/v1/jobs/{job_id}").json()
        if j["status"] == "running":
            seen_running = True
            max_step = max(max_step, j["progress"]["step"])
        if j["status"] in ("done", "failed"):
            assert j["status"] == "done", j.get("error")
            break
        time.sleep(1.0)
    assert seen_running
    assert max_step > 0
