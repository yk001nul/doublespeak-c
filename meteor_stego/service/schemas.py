"""
Request/response models for the async job API.

Design notes:
  * Message and key material cross the wire base64-encoded so arbitrary bytes
    survive JSON. ``message`` (plain UTF-8 text) is offered as a convenience for
    encode; exactly one of ``message`` / ``message_b64`` must be supplied.
  * ``salt_b64`` is REQUIRED and must decode to exactly 32 non-zero bytes — the
    zero-salt CLI shortcut is not acceptable over a network (see
    SERVICE_ARCHITECTURE.md "Security notes").
"""
import base64

from pydantic import BaseModel, Field, field_validator, model_validator

from . import config


def _decode_b64(value: str, field: str) -> bytes:
    try:
        return base64.b64decode(value, validate=True)
    except Exception as exc:  # noqa: BLE001 - surfaced as 422
        raise ValueError(f"{field} is not valid base64") from exc


class _KeyedRequest(BaseModel):
    """Fields shared by encode and decode: the shared secret and topic."""

    starting_context: str = Field(..., min_length=1)
    style: int = Field(default=1, ge=0, le=4,
                       description="MeteorStyle: 1=chat 2=email 3=blog 4=news; 0=legacy")
    key_material_b64: str = Field(
        ..., description="base64 key material (HKDF input). Mutually exclusive with key_raw_b64.")
    key_raw_b64: str | None = Field(
        default=None, description="base64 of a pre-derived 32-byte key.")
    salt_b64: str = Field(..., description="base64 of a 32-byte random salt (required).")

    @model_validator(mode="after")
    def _check_keys_and_salt(self):
        # Exactly one of key_material / key_raw.
        has_material = bool(self.key_material_b64)
        has_raw = bool(self.key_raw_b64)
        if has_material == has_raw:
            raise ValueError("supply exactly one of key_material_b64 or key_raw_b64")
        if has_raw:
            raw = _decode_b64(self.key_raw_b64, "key_raw_b64")
            if len(raw) != 32:
                raise ValueError(f"key_raw_b64 must decode to 32 bytes, got {len(raw)}")

        salt = _decode_b64(self.salt_b64, "salt_b64")
        if len(salt) != config.REQUIRED_SALT_LEN:
            raise ValueError(
                f"salt_b64 must decode to exactly {config.REQUIRED_SALT_LEN} bytes, "
                f"got {len(salt)}")
        if not any(salt):
            raise ValueError("salt must not be all zero bytes (zero salt is rejected)")
        return self

    def key_material(self) -> bytes | None:
        return _decode_b64(self.key_material_b64, "key_material_b64") if self.key_material_b64 else None

    def key_raw(self) -> bytes | None:
        return _decode_b64(self.key_raw_b64, "key_raw_b64") if self.key_raw_b64 else None

    def salt(self) -> bytes:
        return _decode_b64(self.salt_b64, "salt_b64")


class EncodeRequest(_KeyedRequest):
    message: str | None = Field(default=None, description="UTF-8 message text to encode.")
    message_b64: str | None = Field(default=None, description="base64 message bytes to encode.")
    beta: int = Field(default=config.DEFAULT_BETA, ge=2, le=5)
    num_candidates: int = Field(default=config.DEFAULT_NUM_CANDIDATES, ge=4, le=8)
    max_steps: int = Field(default=config.DEFAULT_MAX_STEPS, ge=1)
    llm_timeout_ms: int = Field(default=config.DEFAULT_LLM_TIMEOUT_MS, ge=1000)

    @model_validator(mode="after")
    def _check_message(self):
        if (self.message is None) == (self.message_b64 is None):
            raise ValueError("supply exactly one of message or message_b64")
        return self

    def message_bytes(self) -> bytes:
        if self.message is not None:
            return self.message.encode("utf-8")
        return _decode_b64(self.message_b64, "message_b64")


class DecodeRequest(_KeyedRequest):
    covertext: str = Field(..., min_length=1)
    beta: int = Field(default=config.DEFAULT_BETA, ge=2, le=5)
    num_candidates: int = Field(default=config.DEFAULT_NUM_CANDIDATES, ge=4, le=8)
    max_steps: int = Field(default=config.DEFAULT_MAX_STEPS, ge=1)
    llm_timeout_ms: int = Field(default=config.DEFAULT_LLM_TIMEOUT_MS, ge=1000)


class JobAccepted(BaseModel):
    job_id: str
    status: str


class Progress(BaseModel):
    step: int = 0
    bits_done: int = 0
    total_bits: int = 0
    elapsed_s: float = 0.0
    eta_s: float | None = None


class JobStatus(BaseModel):
    job_id: str
    kind: str
    status: str
    progress: Progress
    result: dict | None = None
    error: str | None = None
    model_info: dict
