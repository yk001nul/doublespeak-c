"""
Admission control and per-caller quota for the public front-end.

The backend is one worker that takes minutes per job, so accepting work is a
promise the service usually cannot keep in a hurry. Three independent gates run
on every submit, cheapest first:

  1. Global admission control — is the backlog already too deep to accept more?
  2. Per-caller concurrency — is this caller already occupying the queue?
  3. Per-caller daily quota — has this caller used its share for the day?

Order matters. The first two are pure reads, so a request rejected by them does
not burn the caller's daily quota; only the last gate mutates state.

All three answer with 429 and a `Retry-After` hint. That is deliberate: telling a
caller to come back later is more honest than accepting a job that would sit
behind hours of backlog with no signal.
"""
import datetime
import logging

from fastapi import HTTPException

from . import settings

log = logging.getLogger("meteor.service.quota")


def utc_day() -> str:
    """The quota bucket key: UTC calendar day, e.g. "2026-07-21"."""
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")


def _too_many(detail: str, retry_after_s: int):
    return HTTPException(status_code=429, detail=detail,
                         headers={"Retry-After": str(retry_after_s)})


class FirestoreUsageCounter:
    """Per-key, per-day job counters in Firestore.

    One document per (key, day) so the counter is a single atomic increment and
    old buckets age out under the collection's TTL policy rather than needing a
    reset job.
    """

    def __init__(self, project: str | None = None, collection: str | None = None):
        from google.cloud import firestore  # lazy
        self._client = firestore.Client(project=project or settings.GCP_PROJECT)
        self._col = self._client.collection(
            collection or settings.USAGE_COLLECTION)

    def increment(self, key_hash: str) -> int:
        """Add one to today's count for this key and return the new total."""
        from google.cloud import firestore  # lazy
        day = utc_day()
        doc = self._col.document(f"{key_hash}_{day}")
        doc.set({"key_hash": key_hash, "day": day,
                 "count": firestore.Increment(1),
                 "expires_at": _expiry(day)}, merge=True)
        snap = doc.get()
        return int(snap.get("count") or 0)


def _expiry(day: str) -> datetime.datetime:
    """When this usage bucket may be deleted — two days after its own day, which
    is comfortably past any timezone's view of "today"."""
    d = datetime.datetime.strptime(day, "%Y-%m-%d").replace(
        tzinfo=datetime.timezone.utc)
    return d + datetime.timedelta(days=2)


def enforce(caller, store, usage) -> None:
    """Run all three gates for `caller`. Raises HTTPException(429) on rejection.

    `store` supplies the two counts (see store.JobStore); `usage` is the daily
    counter. Returns None when the request may proceed.
    """
    # 1. Global backlog. Each queued job is ~4 minutes of worker time, so the
    #    threshold doubles as the worst-case wait we are willing to promise.
    queued = store.count_queued()
    if queued >= settings.ADMISSION_MAX_QUEUED:
        log.warning("admission rejected: backlog=%d caller=%s", queued, caller.short)
        raise _too_many(
            f"service at capacity ({queued} jobs queued); retry later",
            settings.ADMISSION_RETRY_AFTER_S)

    # 2. Per-caller concurrency, so one caller cannot occupy the whole backlog.
    if caller.max_concurrent > 0:
        active = store.count_active_for_owner(caller.key_hash)
        if active >= caller.max_concurrent:
            raise _too_many(
                f"you already have {active} job(s) in flight "
                f"(limit {caller.max_concurrent}); wait for one to finish",
                settings.CONCURRENCY_RETRY_AFTER_S)

    # 3. Daily quota. Last, and the only gate that mutates, so a request turned
    #    away above does not cost the caller anything.
    if caller.quota_daily > 0:
        used = usage.increment(caller.key_hash)
        if used > caller.quota_daily:
            raise _too_many(
                f"daily quota of {caller.quota_daily} jobs exhausted; "
                f"resets at 00:00 UTC",
                settings.QUOTA_RETRY_AFTER_S)
