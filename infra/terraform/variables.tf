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
  default     = "n2-standard-4"
  description = "GKE node machine type; size vCPU to num_threads (CPU-only inference)."
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
