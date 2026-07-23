"""
Operator CLI for API keys.

Run from the repo root, so the module path resolves. Use `python3` on Cloud
Shell and any other Linux host; `py -3` is the Windows launcher and does not
exist there.

    python3 -m meteor_stego.service.gcp.manage_keys create --label alice --email a@b.c
    python3 -m meteor_stego.service.gcp.manage_keys list
    python3 -m meteor_stego.service.gcp.manage_keys disable <key-hash>
    python3 -m meteor_stego.service.gcp.manage_keys enable  <key-hash>
    python3 -m meteor_stego.service.gcp.manage_keys show    <key-hash>

Needs `GCP_PROJECT` set and application-default credentials with Firestore
access. `create` prints the key exactly once — only its SHA-256 is stored, so a
lost key cannot be recovered and must be replaced. `list` therefore shows key
hashes, never keys; to check whether a key you hold is the registered one,
compare `printf '%s' '<key>' | sha256sum` against its hash.

This is the interim issuance path. Self-serve signup (Firebase Auth → mint a key
per Google account) replaces it; the storage format is the same either way.
"""
import argparse
import sys

from . import settings
from .auth import generate_key, hash_key


def _collection():
    from google.cloud import firestore  # lazy
    client = firestore.Client(project=settings.GCP_PROJECT)
    return client.collection(settings.API_KEYS_COLLECTION)


def cmd_create(args) -> int:
    import datetime
    raw = generate_key()
    digest = hash_key(raw)
    _collection().document(digest).set({
        "label": args.label,
        "owner_email": args.email,
        "tier": args.tier,
        "disabled": False,
        "quota_daily": args.quota_daily,
        "max_concurrent": args.max_concurrent,
        "created_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    })
    print(f"key:       {raw}")
    print(f"key_hash:  {digest}")
    print(f"tier:      {args.tier}  quota={args.quota_daily}/day  "
          f"concurrency={args.max_concurrent}")
    print("\nStore the key now — it is not recoverable from the database.")
    return 0


def cmd_list(args) -> int:
    rows = list(_collection().stream())
    if not rows:
        print("no keys")
        return 0
    print(f"{'key_hash':<20} {'label':<20} {'tier':<8} {'quota':>6} {'conc':>5}  state")
    for snap in rows:
        d = snap.to_dict() or {}
        state = "disabled" if d.get("disabled") else "active"
        print(f"{snap.id[:18]:<20} {(d.get('label') or ''):<20} "
              f"{(d.get('tier') or ''):<8} {d.get('quota_daily', ''):>6} "
              f"{d.get('max_concurrent', ''):>5}  {state}")
    return 0


def cmd_show(args) -> int:
    snap = _collection().document(args.key_hash).get()
    if not snap.exists:
        print(f"no such key: {args.key_hash}", file=sys.stderr)
        return 1
    for k, v in sorted((snap.to_dict() or {}).items()):
        print(f"{k:<16} {v}")
    return 0


def _set_disabled(key_hash: str, disabled: bool) -> int:
    doc = _collection().document(key_hash)
    if not doc.get().exists:
        print(f"no such key: {key_hash}", file=sys.stderr)
        return 1
    doc.update({"disabled": disabled})
    print(f"{key_hash} {'disabled' if disabled else 'enabled'}")
    # Front-end instances cache key records, so revocation is not instant.
    print(f"note: takes up to {settings.API_KEY_CACHE_TTL_S:.0f}s to take effect "
          f"(per-instance key cache).")
    return 0


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="manage_keys", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("create", help="mint a new API key")
    c.add_argument("--label", required=True, help="human-readable name")
    c.add_argument("--email", default="", help="owner's email, for contact")
    c.add_argument("--tier", default="free")
    c.add_argument("--quota-daily", type=int,
                   default=settings.FREE_TIER_QUOTA_DAILY)
    c.add_argument("--max-concurrent", type=int,
                   default=settings.FREE_TIER_MAX_CONCURRENT)
    c.set_defaults(func=cmd_create)

    sub.add_parser("list", help="list all keys").set_defaults(func=cmd_list)

    s = sub.add_parser("show", help="show one key record")
    s.add_argument("key_hash")
    s.set_defaults(func=cmd_show)

    d = sub.add_parser("disable", help="revoke a key")
    d.add_argument("key_hash")
    d.set_defaults(func=lambda a: _set_disabled(a.key_hash, True))

    e = sub.add_parser("enable", help="un-revoke a key")
    e.add_argument("key_hash")
    e.set_defaults(func=lambda a: _set_disabled(a.key_hash, False))

    args = p.parse_args(argv)
    if not settings.GCP_PROJECT:
        print("GCP_PROJECT is not set", file=sys.stderr)
        return 2
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
