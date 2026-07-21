variable "project_id" {
  type        = string
  description = "GCP project ID to deploy into."
}

variable "region" {
  type        = string
  default     = "us-central1"
  description = "Primary region for regional resources (Cloud Run, Cloud Tasks, GKE, GCS)."
}

variable "zone" {
  type        = string
  default     = "us-central1-a"
  description = "Zone for the GKE node pool."
}

# ── Determinism contract (served + enforced) ─────────────────────────────────

variable "num_threads" {
  type        = number
  default     = 4
  description = <<-EOT
    METEOR_NUM_THREADS — shared protocol state. Encoder and decoder MUST match.
    Also sizes worker vCPU. Validated values on Phi-3.5-mini: 1 and 4.
  EOT
}

variable "gguf_object" {
  type        = string
  default     = "models/Phi-3.5-mini-instruct-Q4_K_M.gguf"
  description = "GCS object path (within the bucket) of the pinned model."
}

variable "gguf_sha256" {
  type        = string
  description = "SHA-256 of the pinned GGUF. Workers refuse to serve on mismatch."
}

variable "llama_cpp_tag" {
  type        = string
  description = "Pinned llama.cpp git tag the worker image was built from."
}

# ── Scaling ──────────────────────────────────────────────────────────────────

variable "worker_min_replicas" {
  type        = number
  default     = 1
  description = "Keep >= 1 warm: cold start reloads a multi-GB GGUF with --no-mmap."
}

variable "worker_max_replicas" {
  type        = number
  default     = 4
  description = "Upper bound on concurrent single-slot workers."
}

variable "worker_machine_type" {
  type        = string
  default     = "c2-standard-4"
  description = <<-EOT
    GKE node machine type; size vCPU to num_threads (CPU-only inference).
    DETERMINISM: use a single-CPU-platform family so every autoscaled node runs
    byte-identical ggml float math. c2-* is always Intel Cascade Lake, so the
    whole pool is float-identical by construction (4 vCPU / 16 GB — fits threads=4
    + the --no-mmap GGUF). Do NOT use e2-* for a multi-node pool: E2 runs on a mix
    of host CPUs (Broadwell..Ice Lake, Intel or AMD) and does not support
    min_cpu_platform, so different nodes can select different SIMD kernels (AVX2 vs
    AVX-512) and desync encode/decode silently. N2 is an alternative but needs
    node_min_cpu_platform pinned to floor the ISA.
  EOT
}

variable "node_min_cpu_platform" {
  type        = string
  default     = "Intel Cascade Lake"
  description = <<-EOT
    Floors the CPU microarchitecture of every worker node so ggml dispatches the
    SAME SIMD kernel (and thus the same float reduction order) on all nodes —
    required for cross-node encode/decode lockstep once the pool autoscales past
    one node. Matches the c2-* default (always Cascade Lake). For N2 set to
    "Intel Ice Lake" (or the floor you want). Empty string => null (GKE picks; NOT
    determinism-safe, only valid for a permanently single-node pool). Ignored by
    families that don't support it (E2) — another reason not to use E2 here.
  EOT
}

variable "network" {
  type        = string
  default     = "default"
  description = <<-EOT
    VPC network for the GKE cluster. Defaults to the project's auto-created
    "default" network. Projects created with the default-network org policy
    disabled have no "default" VPC — set this (and subnetwork) to an existing
    network, or the cluster apply fails with a network-not-found error.
  EOT
}

variable "subnetwork" {
  type        = string
  default     = ""
  description = <<-EOT
    VPC subnetwork for the GKE node pool. Empty lets GKE auto-select a subnet in
    the cluster region from var.network (valid for auto-mode networks like the
    default VPC). For a custom-mode network, set this to an existing subnet in
    var.region.
  EOT
}

variable "image_frontend" {
  type        = string
  description = "Full Artifact Registry image ref for the front-end (set by Cloud Build)."
  default     = ""
}

variable "worker_url" {
  type        = string
  default     = ""
  description = <<-EOT
    Base URL of the GKE worker Ingress that Cloud Tasks pushes to, and the `aud`
    it signs each OIDC token with. Not known at first apply — set it once the
    worker Ingress + managed cert are up: `https://<host>` where <host> is the
    sslip.io name resolving to the reserved Ingress IP (worker_ingress_ip), e.g.
    https://35-201-65-159.sslip.io. MUST be byte-identical to the ConfigMap's
    WORKER_OIDC_AUDIENCE and the managed cert / Ingress host, or the worker 401s
    every push. Must be https:// — Cloud Tasks refuses to attach an OIDC token to
    a plain-http target. The front-end (gcp/settings.WORKER_URL) requires it and
    refuses to start without it, so leaving it empty is only valid while
    image_frontend is also empty (front-end not yet created).
  EOT
}

variable "image_worker" {
  type        = string
  description = "Full Artifact Registry image ref for the worker (set by Cloud Build)."
  default     = ""
}

# ── Observability + turn-down (DDoS blast-radius control) ─────────────────────

variable "frontend_max_instances" {
  type        = number
  default     = 10
  description = <<-EOT
    Hard ceiling on Cloud Run front-end instances. Bounds cost + blast radius
    under a request flood: past this, Cloud Run queues/sheds excess requests
    instead of scaling (and billing) without limit. Raise for real traffic;
    lower it (or use the kill-switch runbook) during an incident.
  EOT
}

variable "alert_email" {
  type        = string
  default     = ""
  description = <<-EOT
    Email address for Cloud Monitoring alert notifications. Empty => the alert
    policies are still created (visible in the console) but no notification
    channel is attached, so they won't email anyone. Set this to actually get
    paged on a request spike / 5xx surge / queue backlog.
  EOT
}

variable "alert_request_rate_threshold" {
  type        = number
  default     = 20
  description = "Front-end requests/sec (summed across instances) above which the request-spike alert fires — possible abuse/DDoS. Tune to expected traffic."
}

variable "alert_5xx_rate_threshold" {
  type        = number
  default     = 1
  description = "Front-end 5xx responses/sec above which the server-error alert fires."
}

variable "alert_queue_depth_threshold" {
  type        = number
  default     = 25
  description = "Cloud Tasks meteor-jobs backlog depth above which the queue-backlog alert fires (flood, or stalled/insufficient workers)."
}

variable "alert_401_rate_threshold" {
  type        = number
  default     = 1
  description = "Front-end 401s/sec above which the auth-failure alert fires. Legitimate clients either hold a valid key or do not, so a sustained rate means someone is guessing keys."
}

variable "alert_429_rate_threshold" {
  type        = number
  default     = 1
  description = "Front-end 429s/sec (sustained 10 min) above which the quota-rejection alert fires — demand has outgrown the single worker, or one caller is hammering a limit."
}

# ── Go public (opt-in) ───────────────────────────────────────────────────────

variable "frontend_public" {
  type        = bool
  default     = false
  description = <<-EOT
    Expose the front-end on the public internet. Default false so merging this
    changes nothing. When true, Terraform creates an external Application Load
    Balancer + Cloud Armor policy (frontend_lb.tf), grants allUsers the invoker
    role, and pins the Cloud Run service's ingress to the load balancer so its
    *.run.app URL stops answering directly.

    Only flip this once the application-side API key enforcement is DEPLOYED
    (REQUIRE_API_KEY, service/gcp/auth.py). The load balancer authenticates
    nobody — it rate-limits anonymous traffic and nothing more.

    Costs roughly $30/mo: ~$18-25 for the load balancer, ~$8-10 for Cloud Armor
    Standard.
  EOT
}

variable "frontend_domain" {
  type        = string
  default     = ""
  description = <<-EOT
    Hostname for the front-end's Google-managed TLS certificate. Empty => derive
    an sslip.io host from the reserved IP (e.g. 34-1-2-3.sslip.io), which needs
    no owned domain — the same approach the worker Ingress uses.

    To move to a real domain: register it, point an A record at the
    frontend_ip output, set this variable, and re-apply. Nothing else changes.
  EOT
}

variable "armor_allowed_ips" {
  type        = list(string)
  default     = []
  description = <<-EOT
    Staged-rollout allowlist. While this is non-empty, the Cloud Armor policy
    DENIES everything except these CIDR ranges, so the whole public stack can be
    deployed and tested end to end before it is genuinely open. Set it to your
    own address for the first apply, then empty it to open the service.
  EOT
}

variable "armor_rate_limit_rpm" {
  type        = number
  default     = 120
  description = <<-EOT
    Per-IP requests per minute allowed by Cloud Armor before returning 429.
    Keep this generous: a client polls /v1/jobs/{id} every few seconds for the
    whole multi-minute life of a job, so a tight limit breaks normal use. Fair
    sharing between callers is the API key quota's job, not this one.
  EOT
}

variable "armor_waf_preview" {
  type        = bool
  default     = true
  description = <<-EOT
    Attach the preconfigured SQLi/XSS WAF signatures in PREVIEW mode (they log
    matches but never block). This API carries base64 key material and covertext
    in JSON bodies, which trips those signatures on perfectly legitimate
    requests — review the preview hits in Logging before ever promoting them to
    enforcing.
  EOT
}

variable "admission_max_queued" {
  type        = number
  default     = 10
  description = <<-EOT
    Refuse new jobs (429 + Retry-After) once this many are already queued. One
    worker at ~4 minutes per job means 10 queued is already a ~40 minute wait —
    past that a caller is better served by being told to come back than by
    being handed a job id that sits for hours.
  EOT
}

variable "free_tier_quota_daily" {
  type        = number
  default     = 5
  description = <<-EOT
    Jobs per UTC day per API key, unless the key's own record overrides it.
    Total capacity is ~300 jobs/day, so 5/day supports roughly 60 active
    callers.
  EOT
}

variable "free_tier_max_concurrent" {
  type        = number
  default     = 1
  description = "Jobs one API key may have queued or running at once, so a single caller cannot occupy the whole backlog."
}

variable "billing_account" {
  type        = string
  default     = ""
  description = <<-EOT
    Billing account ID (e.g. "012345-678901") used to create a budget alert.
    Empty => no budget is created. Strongly recommended before going public: the
    budget is the backstop that tells you a public endpoint is costing money
    faster than expected. Requires roles/billing.admin on the account.
  EOT
}

variable "budget_amount_usd" {
  type        = number
  default     = 400
  description = "Monthly budget in USD. Alerts fire at 50%, 90% and 100% of this."
}

# ── Spot / preemptible worker nodes (opt-in) ─────────────────────────────────

variable "worker_use_spot" {
  type        = bool
  default     = false
  description = <<-EOT
    Run the worker node pool on Spot VMs (~60-70% cheaper than on-demand). Default
    false so merging changes nothing; flip true on an apply to move the pool to
    Spot. NOTE: toggling this recreates the node pool (Spot is a pool-level
    attribute), so expect a brief worker outage + GGUF reload on the new nodes when
    you change it — do it in a maintenance window.

    DETERMINISM: safe. Spot nodes are the SAME machine_type (c2-standard-4) with the
    SAME node_min_cpu_platform floor as on-demand, so ggml dispatches the identical
    SIMD kernel and float reduction order — encode/decode stay byte-locked. Spot
    changes only price/availability, not float math.

    RELIABILITY trade-off: GCE can preempt a Spot node on ~30s notice. A job running
    on a preempted node dies mid-flight. This is NOT auto-recovered by Cloud Tasks:
    the ack-fast worker returns 200 before running the job in the background, so the
    task is already gone from the queue and never retried. Recovery is explicit — on
    SIGTERM the worker resets its in-flight job to queued and re-enqueues a fresh
    push (worker_app.recover_inflight), so a replacement worker re-runs it (jobs are
    deterministic => identical output). That path needs the worker's Cloud Tasks
    enqueuer + act-as-invoker IAM and TASKS_QUEUE/WORKER_URL config (both added
    alongside this var). Costs are still: (a) the preempted job restarts from scratch
    (adds its whole runtime again, minutes), and (b) the replacement node cold-reloads
    the multi-GB --no-mmap GGUF before it can serve. Also, Spot capacity is not
    guaranteed: under c2 Spot shortage the autoscaler may be unable to get a node and
    jobs wait. Keep worker_min_replicas >= 1 so at least one node is (best-effort)
    always warm. Prefer this once traffic can absorb the occasional restart; keep it
    false if you need every job to finish on its first attempt.
  EOT
}

# ── Scheduled cost scaling (interim; opt-in) ─────────────────────────────────

variable "cost_schedule_enabled" {
  type        = bool
  default     = false
  description = <<-EOT
    Opt-in daily scale-down of the C2 worker pool to 0 nodes during idle hours
    (Cloud Run job driven by two Cloud Scheduler crons; see cost_schedule.tf).
    Default false: none of the scaler resources are created until you set this
    true on a terraform apply. Only enable once you know the traffic has a
    genuinely idle window — during the down window the queue is paused and jobs
    wait until scale-up. Test the job by hand before trusting the schedule
    (infra/RUNBOOK.md).
  EOT
}

variable "scale_down_cron" {
  type        = string
  default     = "0 0 * * *"
  description = "Cron (in schedule_timezone) to scale the worker pool to 0 + pause the queue. Default 00:00."
}

variable "scale_up_cron" {
  type        = string
  default     = "0 8 * * *"
  description = "Cron (in schedule_timezone) to scale the worker pool back to worker_min_replicas + resume the queue. Default 08:00. Allow lead time before demand for the node provision + GGUF reload (~a few min)."
}

variable "schedule_timezone" {
  type        = string
  default     = "Etc/UTC"
  description = "IANA timezone for the scale up/down crons (e.g. \"Asia/Singapore\", \"America/New_York\"). Set to your users' local time so the idle window lines up."
}
