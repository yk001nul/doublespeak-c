"""
API key authentication for the public front-end.

The front-end is the only public door to a service whose backend is a single
worker costing real money per job, so every request must be attributable to a
caller. Keys are bearer credentials: whoever holds one is the caller.

Storage model: the key itself is never persisted. The Firestore document ID is
the SHA-256 hex of the key, so a database leak does not yield usable keys, and a
lookup is a single point read with no scan and no string comparison to time.

Testability follows the same `app.state` injection the worker's OIDC check uses:
the routes resolve their key store from `app.state.api_keys`, so unit tests can
substitute a fake without any google-cloud library installed.
"""
import hashlib
import logging
import secrets
import time
from dataclasses import dataclass, field

from fastapi import HTTPException, Request

from . import settings

log = logging.getLogger("meteor.service.auth")

KEY_PREFIX = "dsk_live_"


def generate_key() -> str:
    """Mint a new API key. Returned once to the caller and never stored."""
    return KEY_PREFIX + secrets.token_urlsafe(32)


def hash_key(raw: str) -> str:
    """The stored identity of a key: its SHA-256 hex digest."""
    return hashlib.sha256(raw.encode("utf-8")).hexdigest()


@dataclass
class Caller:
    """An authenticated API key and the limits that apply to it."""

    key_hash: str
    label: str = ""
    owner_email: str = ""
    tier: str = "free"
    disabled: bool = False
    quota_daily: int = field(default_factory=lambda: settings.FREE_TIER_QUOTA_DAILY)
    max_concurrent: int = field(default_factory=lambda: settings.FREE_TIER_MAX_CONCURRENT)

    @property
    def short(self) -> str:
        """A log-safe identifier — enough to correlate abuse, not enough to
        reconstruct the key."""
        return self.key_hash[:12]


def _caller_from_dict(key_hash: str, d: dict) -> Caller:
    return Caller(
        key_hash=key_hash,
        label=d.get("label") or "",
        owner_email=d.get("owner_email") or "",
        tier=d.get("tier") or "free",
        disabled=bool(d.get("disabled")),
        quota_daily=int(d.get("quota_daily") or settings.FREE_TIER_QUOTA_DAILY),
        max_concurrent=int(
            d.get("max_concurrent") or settings.FREE_TIER_MAX_CONCURRENT),
    )


class FirestoreApiKeyStore:
    """Key records in Firestore, with a short in-process cache.

    Clients poll `/v1/jobs/{id}` for the whole multi-minute life of a job, so an
    uncached lookup would bill a Firestore read every few seconds per active
    caller. The cache TTL is the window in which a revoked key still works, so
    keep it short — and note that revocation is not instant across Cloud Run
    instances regardless, since each holds its own cache.
    """

    def __init__(self, project: str | None = None, collection: str | None = None,
                 cache_ttl_s: float | None = None):
        from google.cloud import firestore  # lazy
        self._client = firestore.Client(project=project or settings.GCP_PROJECT)
        self._col = self._client.collection(
            collection or settings.API_KEYS_COLLECTION)
        self._ttl = settings.API_KEY_CACHE_TTL_S if cache_ttl_s is None else cache_ttl_s
        self._cache: dict[str, tuple[float, Caller | None]] = {}

    def lookup(self, key_hash: str) -> Caller | None:
        now = time.monotonic()
        hit = self._cache.get(key_hash)
        if hit is not None and hit[0] > now:
            return hit[1]
        snap = self._col.document(key_hash).get()
        caller = _caller_from_dict(key_hash, snap.to_dict()) if snap.exists else None
        # Negative results are cached too, so a flood of guessed keys cannot turn
        # into a flood of Firestore reads.
        self._cache[key_hash] = (now + self._ttl, caller)
        return caller


def extract_key(request: Request) -> str | None:
    """Pull the key from either accepted header form.

    X-API-Key is checked FIRST, and that order matters. While the service is
    IAM-private, Cloud Run requires `Authorization: Bearer <Google identity
    token>` and forwards that header to the container — so if Authorization won,
    every request would authenticate the *identity token* as an API key, fail
    the hash lookup, and 401 with "invalid API key" no matter how valid the real
    key was. The caller has no way out of that: dropping Authorization to free
    it up means Cloud Run rejects the request before it ever reaches us.

    Checking X-API-Key first makes the two schemes compose: IAM owns
    Authorization, the application owns X-API-Key, and a caller can send both.
    Bearer is still accepted as a fallback, which is the ergonomic form once the
    front-end is public and no identity token is in play.
    """
    key = request.headers.get("x-api-key")
    if key:
        return key
    header = request.headers.get("authorization") or ""
    if header.lower().startswith("bearer "):
        return header[7:].strip() or None
    return None


def require_api_key(request: Request) -> Caller:
    """FastAPI dependency: resolve and validate the caller's API key.

    Every rejection is a flat 401 with the same detail — an unknown key and a
    disabled key are deliberately indistinguishable, so the endpoint cannot be
    used to probe which keys exist.
    """
    if not settings.REQUIRE_API_KEY:
        return Caller(key_hash="anonymous", label="auth-disabled")

    store = getattr(request.app.state, "api_keys", None)
    if store is None:
        # Fail closed: an unwired key store must never mean "let everyone in".
        log.error("api key store not configured; refusing request")
        raise HTTPException(status_code=503, detail="auth unavailable")

    raw = extract_key(request)
    if not raw:
        raise HTTPException(
            status_code=401, detail="missing API key",
            headers={"WWW-Authenticate": "Bearer"})

    caller = store.lookup(hash_key(raw))
    if caller is None or caller.disabled:
        raise HTTPException(status_code=401, detail="invalid API key")
    return caller
