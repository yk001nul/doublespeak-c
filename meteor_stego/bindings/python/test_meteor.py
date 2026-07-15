"""
Tests for the Python ctypes binding (meteor.py).

Two groups:
  * Fast, no server — struct lockstep, argument validation, error mapping,
    the heuristic capacity estimate. Always run.
  * Server-backed — style round trips + progress callback. Skipped unless a
    llama-server is reachable at METEOR_LLM_URL (default http://127.0.0.1:8080).

Run:  py -3 -m pytest meteor_stego/bindings/python/test_meteor.py -v
"""
import ctypes
import os

import pytest

import meteor
from meteor import (
    Meteor, MeteorStyle,
    MeteorError, MeteorConfigError, MeteorCapacityError,
    _MeteorConfig, _MeteorCapacityEstimate,
)

LLM_URL = os.environ.get("METEOR_LLM_URL", "http://127.0.0.1:8080")


def _server_reachable():
    try:
        return Meteor(key_input=b"probe", llm_url=LLM_URL).health()
    except Exception:
        return False


needs_server = pytest.mark.skipif(
    not _server_reachable(), reason="llama-server not reachable"
)


# ── Fast, no-server ─────────────────────────────────────────────────────────

def test_style_enum_values():
    # Must match MeteorStyle in meteor.h.
    assert (MeteorStyle.NONE, MeteorStyle.INFORMAL_CHAT, MeteorStyle.FORMAL_EMAIL,
            MeteorStyle.CASUAL_BLOG, MeteorStyle.NEWS_ARTICLE) == (0, 1, 2, 3, 4)


def test_config_struct_field_order():
    # T7 lockstep guard: field order/names must match MeteorConfig in meteor.h,
    # with `style` last.
    names = [f[0] for f in _MeteorConfig._fields_]
    assert names == [
        "key_raw", "key_input", "key_input_len", "salt", "salt_len",
        "beta", "num_candidates", "llm_url", "hyphen_dict",
        "max_steps", "llm_timeout_ms", "style",
    ]
    assert names[-1] == "style"


def test_capacity_struct_fields():
    names = [f[0] for f in _MeteorCapacityEstimate._fields_]
    assert names == ["estimated_bits", "estimated_bytes", "estimated_words",
                     "avg_bits_per_word", "sample_steps_used"]


def test_requires_exactly_one_key():
    with pytest.raises(ValueError):
        Meteor()  # neither
    with pytest.raises(ValueError):
        Meteor(key_input=b"x", key_raw=b"y" * 32)  # both


def test_key_raw_length_validated():
    with pytest.raises(ValueError):
        Meteor(key_raw=b"too-short")


def test_error_code_mapping():
    # Every documented code maps to a MeteorError subclass carrying .code.
    for code in (meteor.METEOR_ERR_CONFIG, meteor.METEOR_ERR_LLM,
                 meteor.METEOR_ERR_CAPACITY, meteor.METEOR_ERR_DECODE,
                 meteor.METEOR_ERR_DICT, meteor.METEOR_ERR_OOM,
                 meteor.METEOR_ERR_TIMEOUT, meteor.METEOR_ERR_CRYPTO):
        with pytest.raises(MeteorError) as ei:
            meteor._raise_for_code(code, "test")
        assert ei.value.code == code
    # OK is a no-op.
    assert meteor._raise_for_code(meteor.METEOR_OK, "test") is None


def test_capacity_error_is_subclass():
    assert issubclass(MeteorCapacityError, MeteorError)


def test_heuristic_capacity_no_server():
    # sample_steps=0 must not touch the network. Capacity scales with context
    # length (est_bits = words * style_expansion * beta * 0.65), so use a
    # context long enough to clear the 8-bits-per-byte floor.
    m = Meteor(key_input=b"secret", style=MeteorStyle.NEWS_ARTICLE, llm_url=LLM_URL)
    ctx = ("The government announced sweeping new environmental policies today "
           "aimed at reducing national carbon emissions substantially before "
           "the twenty thirty international climate deadline arrives.")
    est = m.estimate_capacity(ctx, sample_steps=0)
    assert est["estimated_bits"] > 0
    assert est["estimated_bytes"] > 0
    assert est["sample_steps_used"] == 0
    assert est["avg_bits_per_word"] == pytest.approx(3 * 0.65, rel=1e-3)


# ── Server-backed ───────────────────────────────────────────────────────────

@needs_server
@pytest.mark.parametrize("style", [
    MeteorStyle.INFORMAL_CHAT, MeteorStyle.FORMAL_EMAIL,
    MeteorStyle.CASUAL_BLOG, MeteorStyle.NEWS_ARTICLE,
])
def test_style_roundtrip(style):
    # Style mode fills all 2^beta slots only with num_candidates=8 (beta=3),
    # matching the styled_encode ctest; the binding default of 6 is a
    # syllable-mode default.
    m = Meteor(key_input=b"shared-secret", style=style, num_candidates=8,
               llm_url=LLM_URL)
    ctx = "The team will present the quarterly results on Friday."
    cover = m.encode(b"hi", ctx)
    assert m.decode(cover, ctx) == b"hi"


@needs_server
def test_progress_callback_and_determinism():
    m = Meteor(key_input=b"shared-secret", style=MeteorStyle.NEWS_ARTICLE,
               num_candidates=8, llm_url=LLM_URL)
    ctx = "The government announced new climate policies."

    steps = []
    cover_cb = m.encode(b"hi", ctx, progress=lambda s, d, t: steps.append(s))
    cover_plain = m.encode(b"hi", ctx)

    assert steps, "callback was never invoked"
    assert steps == sorted(steps), "step counter must be non-decreasing"
    # The callback must not perturb the covertext (determinism).
    assert cover_cb == cover_plain
