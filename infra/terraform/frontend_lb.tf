# ── Public front door: external Application LB + Cloud Armor ─────────────────
#
# Everything in this file is gated on var.frontend_public (default false), the
# same opt-in pattern as cost_schedule.tf and worker_use_spot: merging changes
# nothing until the flag is flipped.
#
# Topology when enabled:
#
#   Internet → global external Application LB (HTTPS, managed cert)
#            → Cloud Armor policy (per-IP rate limit, WAF rules in preview)
#            → serverless NEG → Cloud Run doublespeak-frontend
#
# The Cloud Run service is simultaneously pinned to
# INGRESS_TRAFFIC_INTERNAL_LOAD_BALANCER (see main.tf), so its *.run.app URL
# stops answering and the load balancer becomes the only way in. Without that,
# an attacker just addresses Cloud Run directly and Cloud Armor never sees the
# request.
#
# Authentication is NOT done here. IAM is opened to allUsers because the real
# check is the per-request API key enforced by the application
# (service/gcp/auth.py); Cloud Armor's job is only to absorb anonymous floods
# before they reach billable Cloud Run instances.

locals {
  # The LB is pointless without a front-end to point at, so it follows the same
  # count-gate as the service itself.
  frontend_lb = var.frontend_public && var.image_frontend != "" ? 1 : 0

  frontend_ip = one(google_compute_global_address.frontend[*].address)

  # A Google-managed cert needs a hostname. With no owned domain, reuse the
  # sslip.io trick already used for the worker Ingress: the host resolves to the
  # reserved IP, so the cert provisions with nothing to register. Swap in a real
  # domain by setting var.frontend_domain and pointing an A record at this IP.
  frontend_host = var.frontend_domain != "" ? var.frontend_domain : (
    local.frontend_ip != null ? "${replace(local.frontend_ip, ".", "-")}.sslip.io" : ""
  )

  # Staged rollout: while armor_allowed_ips is non-empty the policy denies
  # everything except those ranges, so the whole stack can be deployed and
  # tested end to end before it is genuinely open. Empty => open to the world,
  # subject to the rate limit.
  armor_locked_down = length(var.armor_allowed_ips) > 0
}

resource "google_compute_global_address" "frontend" {
  count = local.frontend_lb
  name  = "doublespeak-frontend-ip"
}

# ── Cloud Armor ──────────────────────────────────────────────────────────────
resource "google_compute_security_policy" "frontend" {
  count       = local.frontend_lb
  name        = "doublespeak-frontend-policy"
  description = "Public front door: per-IP rate limiting and WAF preview rules."

  # Staged-rollout allowlist. Highest precedence so it wins over the rate limit.
  dynamic "rule" {
    for_each = local.armor_locked_down ? [1] : []
    content {
      action      = "allow"
      priority    = 500
      description = "Staged rollout: operator IPs only."
      match {
        versioned_expr = "SRC_IPS_V1"
        config {
          src_ip_ranges = var.armor_allowed_ips
        }
      }
    }
  }

  # Per-IP rate limit. Deliberately generous: a client polls /v1/jobs/{id}
  # every couple of seconds for the whole multi-minute life of a job, so a tight
  # limit breaks ordinary use. Per-caller fairness is the API key quota's job
  # (service/gcp/quota.py); this rule only exists to stop anonymous floods.
  rule {
    action      = "throttle"
    priority    = 1000
    description = "Per-IP rate limit."
    match {
      versioned_expr = "SRC_IPS_V1"
      config {
        src_ip_ranges = ["*"]
      }
    }
    rate_limit_options {
      conform_action = "allow"
      exceed_action  = "deny(429)"
      enforce_on_key = "IP"
      rate_limit_threshold {
        count        = var.armor_rate_limit_rpm
        interval_sec = 60
      }
    }
  }

  # Preconfigured WAF signatures, PREVIEW ONLY by default: this API carries
  # base64 key material and covertext in JSON bodies, which trips SQLi/XSS
  # signatures constantly. Watch the preview hits in Logging before promoting
  # any of these to enforcing, or legitimate requests will start 403ing.
  dynamic "rule" {
    for_each = var.armor_waf_preview ? {
      "sqli-v33-stable" = 1200
      "xss-v33-stable"  = 1210
    } : {}
    content {
      action      = "deny(403)"
      priority    = rule.value
      preview     = true
      description = "WAF ${rule.key} (preview — logs only, does not block)."
      match {
        expr {
          expression = "evaluatePreconfiguredExpr('${rule.key}')"
        }
      }
    }
  }

  # Default rule. Required by the API and must be the lowest precedence.
  rule {
    action      = local.armor_locked_down ? "deny(403)" : "allow"
    priority    = 2147483647
    description = local.armor_locked_down ? "Default deny (staged rollout)." : "Default allow."
    match {
      versioned_expr = "SRC_IPS_V1"
      config {
        src_ip_ranges = ["*"]
      }
    }
  }
}

# ── Load balancer ────────────────────────────────────────────────────────────
resource "google_compute_region_network_endpoint_group" "frontend" {
  count                 = local.frontend_lb
  name                  = "doublespeak-frontend-neg"
  region                = var.region
  network_endpoint_type = "SERVERLESS"

  cloud_run {
    service = google_cloud_run_v2_service.frontend[0].name
  }
}

resource "google_compute_backend_service" "frontend" {
  count                 = local.frontend_lb
  name                  = "doublespeak-frontend-backend"
  load_balancing_scheme = "EXTERNAL_MANAGED"
  security_policy       = google_compute_security_policy.frontend[0].id

  backend {
    group = google_compute_region_network_endpoint_group.frontend[0].id
  }

  # Full-sample request logging: with a few hundred jobs a day the volume is
  # trivial, and it is the only per-request record of who called what.
  log_config {
    enable      = true
    sample_rate = 1.0
  }
}

resource "google_compute_managed_ssl_certificate" "frontend" {
  count = local.frontend_lb
  name  = "doublespeak-frontend-cert"

  managed {
    domains = [local.frontend_host]
  }

  # The cert is named, not versioned; changing the domain needs a new resource
  # before the old one can go, or the proxy is briefly left without a cert.
  lifecycle {
    create_before_destroy = true
  }
}

resource "google_compute_url_map" "frontend" {
  count           = local.frontend_lb
  name            = "doublespeak-frontend-urlmap"
  default_service = google_compute_backend_service.frontend[0].id
}

resource "google_compute_target_https_proxy" "frontend" {
  count            = local.frontend_lb
  name             = "doublespeak-frontend-https-proxy"
  url_map          = google_compute_url_map.frontend[0].id
  ssl_certificates = [google_compute_managed_ssl_certificate.frontend[0].id]
}

resource "google_compute_global_forwarding_rule" "frontend_https" {
  count                 = local.frontend_lb
  name                  = "doublespeak-frontend-https"
  load_balancing_scheme = "EXTERNAL_MANAGED"
  target                = google_compute_target_https_proxy.frontend[0].id
  ip_address            = google_compute_global_address.frontend[0].id
  port_range            = "443"
}

# Plain HTTP exists only to redirect. An API served over HTTP would leak the
# caller's API key and key material to any observer, so nothing is served there.
resource "google_compute_url_map" "frontend_redirect" {
  count = local.frontend_lb
  name  = "doublespeak-frontend-redirect"

  default_url_redirect {
    https_redirect         = true
    redirect_response_code = "MOVED_PERMANENTLY_DEFAULT"
    strip_query            = false
  }
}

resource "google_compute_target_http_proxy" "frontend_redirect" {
  count   = local.frontend_lb
  name    = "doublespeak-frontend-http-proxy"
  url_map = google_compute_url_map.frontend_redirect[0].id
}

resource "google_compute_global_forwarding_rule" "frontend_http" {
  count                 = local.frontend_lb
  name                  = "doublespeak-frontend-http"
  load_balancing_scheme = "EXTERNAL_MANAGED"
  target                = google_compute_target_http_proxy.frontend_redirect[0].id
  ip_address            = google_compute_global_address.frontend[0].id
  port_range            = "80"
}

# ── Public invoker ───────────────────────────────────────────────────────────
# Opening IAM is what makes the service reachable without a Google identity
# token. It is safe only because the application requires an API key on every
# route (REQUIRE_API_KEY, set on the service in main.tf) and because ingress is
# simultaneously restricted to the load balancer. Removing this binding is the
# instant kill-switch documented in RUNBOOK.md §2 lever C.
resource "google_cloud_run_v2_service_iam_member" "frontend_public" {
  count    = local.frontend_lb
  project  = var.project_id
  location = var.region
  name     = google_cloud_run_v2_service.frontend[0].name
  role     = "roles/run.invoker"
  member   = "allUsers"
}
