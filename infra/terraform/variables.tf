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
    Base URL of the GKE worker Service that Cloud Tasks pushes to. Not known at
    first apply — set it on the second apply once the worker Service/Ingress has
    an address. The front-end (gcp/settings.WORKER_URL) requires it and refuses
    to start without it, so leaving it empty is only valid while image_frontend
    is also empty (front-end not yet created).
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
