"""
OIDC verification for Cloud Tasks pushes to the worker.

The worker's `POST /internal/run` is reachable over a public HTTP load balancer
so Cloud Tasks can deliver to it (Cloud Tasks only targets public URLs). That
makes token validation mandatory: every push must carry a Google-signed OIDC
token minted for the configured Cloud Tasks invoker service account
(`cloud_tasks.py` sets `service_account_email=WORKER_OIDC_SA`,
`audience=WORKER_URL`), and the worker verifies it here before doing any work.

`google.oauth2` / `google.auth` are imported lazily so this module — and the
worker app that imports it — load without the google libraries for the
fake-backed unit tests. A missing/malformed Authorization header is rejected
before any google import, so those paths stay dependency-free too.
"""
import logging

log = logging.getLogger("meteor.service.worker.oidc")


class OIDCError(Exception):
    """An incoming request's OIDC token is missing or invalid.

    `status` is the HTTP code to return: 401 for a missing/undecodable token,
    403 for a well-formed token from the wrong identity.
    """

    def __init__(self, message: str, status: int = 401):
        super().__init__(message)
        self.status = status


def verify_bearer_token(authorization, expected_audience: str, allowed_sa: str) -> dict:
    """Verify a ``Bearer <jwt>`` Authorization header.

    Returns the decoded token claims on success; raises ``OIDCError`` otherwise.
    Checks, in order: the header is a bearer token; the JWT signature + audience
    + expiry (via Google's public certs); the verified email equals
    ``allowed_sa``.
    """
    if not authorization or not authorization.lower().startswith("bearer "):
        raise OIDCError("missing or malformed Authorization bearer token", status=401)
    token = authorization.split(" ", 1)[1].strip()
    if not token:
        raise OIDCError("empty bearer token", status=401)

    from google.oauth2 import id_token                       # lazy
    from google.auth.transport import requests as ga_requests  # lazy

    try:
        claims = id_token.verify_oauth2_token(
            token, ga_requests.Request(), audience=expected_audience)
    except Exception as exc:  # noqa: BLE001 — signature/audience/expiry failures
        raise OIDCError(f"token verification failed: {exc}", status=401) from exc

    email = claims.get("email", "")
    if allowed_sa and email != allowed_sa:
        raise OIDCError(f"token identity {email!r} is not the expected caller",
                        status=403)
    if not claims.get("email_verified", False):
        raise OIDCError("token email is not verified", status=403)
    return claims
