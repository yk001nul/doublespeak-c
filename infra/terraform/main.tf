# Phase 2 GCP topology for the doublespeak stego service.
#
# Cloud Run front-end  --enqueue-->  Cloud Tasks  --push-->  GKE worker pool
#        |  ^                                                     |
#     write | read                                          progress/result
#        v  |                                                     v
#      Firestore (job source of truth)  <----------------  Firestore
#      GCS (pinned model + large payloads)  <-----------  GCS
#
# NOTE: apply-later. Enable the required APIs (run.googleapis.com,
# cloudtasks.googleapis.com, firestore.googleapis.com, container.googleapis.com,
# artifactregistry.googleapis.com, secretmanager.googleapis.com,
# monitoring.googleapis.com) before apply, or add google_project_service blocks.

locals {
  labels = { app = "doublespeak-stego" }
}

# ── Artifact Registry (determinism-critical image identity) ──────────────────
resource "google_artifact_registry_repository" "images" {
  location      = var.region
  repository_id = "doublespeak"
  format        = "DOCKER"
  description   = "Front-end + worker images (version-locked to meteor.so)."
  labels        = local.labels
}

# ── GCS: pinned model + large payloads ───────────────────────────────────────
resource "google_storage_bucket" "assets" {
  name                        = "${var.project_id}-doublespeak-assets"
  location                    = var.region
  uniform_bucket_level_access = true
  force_destroy               = false
  labels                      = local.labels

  versioning { enabled = true } # auditable model versioning
}

# ── Firestore (native mode): job source of truth ─────────────────────────────
resource "google_firestore_database" "jobs" {
  project     = var.project_id
  name        = "(default)"
  location_id = var.region
  type        = "FIRESTORE_NATIVE"
}

# ── Cloud Tasks: dispatch queue to the single-slot workers ───────────────────
resource "google_cloud_tasks_queue" "jobs" {
  name     = "meteor-jobs"
  location = var.region

  rate_limits {
    # Serialize to the worker pool: at most one dispatch in flight per worker.
    max_concurrent_dispatches = var.worker_max_replicas
    max_dispatches_per_second = 1
  }

  retry_config {
    # Long jobs + ack-fast worker: retries are safe (idempotent), so allow many.
    max_attempts  = 10
    min_backoff   = "10s"
    max_backoff   = "300s"
    max_doublings = 4
  }
}

# ── Service accounts ─────────────────────────────────────────────────────────
resource "google_service_account" "frontend" {
  account_id   = "doublespeak-frontend"
  display_name = "doublespeak front-end (Cloud Run)"
}

resource "google_service_account" "worker" {
  account_id   = "doublespeak-worker"
  display_name = "doublespeak worker (GKE)"
}

# Cloud Tasks uses this identity to mint the OIDC token that authenticates the
# push to the worker.
resource "google_service_account" "tasks_invoker" {
  account_id   = "doublespeak-tasks-invoker"
  display_name = "doublespeak Cloud Tasks -> worker invoker"
}

# ── IAM (least privilege) ────────────────────────────────────────────────────
# Front-end: enqueue tasks, read/write Firestore, read GCS.
resource "google_project_iam_member" "frontend_tasks" {
  project = var.project_id
  role    = "roles/cloudtasks.enqueuer"
  member  = "serviceAccount:${google_service_account.frontend.email}"
}

resource "google_project_iam_member" "frontend_firestore" {
  project = var.project_id
  role    = "roles/datastore.user"
  member  = "serviceAccount:${google_service_account.frontend.email}"
}

# Front-end must be able to act-as the tasks invoker SA to attach its OIDC token.
resource "google_service_account_iam_member" "frontend_actas_invoker" {
  service_account_id = google_service_account.tasks_invoker.name
  role               = "roles/iam.serviceAccountUser"
  member             = "serviceAccount:${google_service_account.frontend.email}"
}

# Worker: read/write Firestore, read model + write results in GCS.
resource "google_project_iam_member" "worker_firestore" {
  project = var.project_id
  role    = "roles/datastore.user"
  member  = "serviceAccount:${google_service_account.worker.email}"
}

resource "google_storage_bucket_iam_member" "worker_gcs" {
  bucket = google_storage_bucket.assets.name
  role   = "roles/storage.objectAdmin"
  member = "serviceAccount:${google_service_account.worker.email}"
}

# ── Secret Manager: long-lived shared stego keys / API keys ──────────────────
resource "google_secret_manager_secret" "api_keys" {
  secret_id = "doublespeak-api-keys"
  replication {
    auto {}
  }
  labels = local.labels
}

# ── GKE: worker pool (CPU-only, warm) ────────────────────────────────────────
resource "google_container_cluster" "workers" {
  name     = "doublespeak-workers"
  location = var.zone

  # Defaults to the project's "default" VPC; override for projects without one
  # (default-network org policy disabled) or a custom-mode network.
  network    = var.network
  subnetwork = var.subnetwork != "" ? var.subnetwork : null

  remove_default_node_pool = true
  initial_node_count       = 1
  deletion_protection      = false

  workload_identity_config { workload_pool = "${var.project_id}.svc.id.goog" }

  # Cluster create/delete regularly exceeds the provider's default window when
  # the zone is under capacity pressure; give it headroom instead of tainting.
  timeouts {
    create = "45m"
    update = "45m"
    delete = "45m"
  }
}

resource "google_container_node_pool" "worker_pool" {
  name     = "worker-pool"
  location = var.zone
  cluster  = google_container_cluster.workers.name

  autoscaling {
    min_node_count = var.worker_min_replicas
    max_node_count = var.worker_max_replicas
  }

  node_config {
    machine_type = var.worker_machine_type # size vCPU to num_threads

    # DETERMINISM: floor the CPU platform so every node the autoscaler creates
    # runs the same ggml SIMD kernel / float reduction order. Without this a
    # second node on a different host CPU desyncs encode/decode silently. Empty
    # => null (unpinned; only safe for a permanently single-node pool).
    min_cpu_platform = var.node_min_cpu_platform != "" ? var.node_min_cpu_platform : null

    oauth_scopes = ["https://www.googleapis.com/auth/cloud-platform"]
    labels       = local.labels

    workload_metadata_config { mode = "GKE_METADATA" }
  }

  # Pool creation blocks until min_node_count nodes are Ready — under zone
  # capacity pressure that can outlast the default timeout and taint the pool.
  timeouts {
    create = "45m"
    update = "45m"
    delete = "45m"
  }
}

# Reserved global static IP for the worker Ingress (external HTTP LB). The k8s
# Ingress references this by name (kubernetes.io/ingress.global-static-ip-name =
# "doublespeak-worker-ip"), so the worker's public address is stable across
# Ingress recreation. Feed http://<this-ip> back in as -var worker_url on the
# 3rd apply, and as WORKER_OIDC_AUDIENCE in the worker ConfigMap.
resource "google_compute_global_address" "worker_ingress" {
  name = "doublespeak-worker-ip"
}

# Bind the k8s worker service account to the GCP worker SA (Workload Identity).
resource "google_service_account_iam_member" "worker_wi" {
  service_account_id = google_service_account.worker.name
  role               = "roles/iam.workloadIdentityUser"
  member             = "serviceAccount:${var.project_id}.svc.id.goog[doublespeak/doublespeak-worker]"
}

# ── Cloud Run: front-end ─────────────────────────────────────────────────────
resource "google_cloud_run_v2_service" "frontend" {
  name     = "doublespeak-frontend"
  location = var.region
  count    = var.image_frontend == "" ? 0 : 1

  template {
    service_account = google_service_account.frontend.email
    scaling { min_instance_count = 0 } # front-end can scale to zero

    containers {
      image = var.image_frontend

      env {
        name  = "GCP_PROJECT"
        value = var.project_id
      }
      env {
        name  = "TASKS_QUEUE"
        value = google_cloud_tasks_queue.jobs.name
      }
      env {
        name  = "TASKS_LOCATION"
        value = var.region
      }
      env {
        name  = "GCS_BUCKET"
        value = google_storage_bucket.assets.name
      }
      env {
        name  = "GCS_MODEL_OBJECT"
        value = var.gguf_object
      }
      env {
        name  = "METEOR_GGUF_SHA256"
        value = var.gguf_sha256
      }
      env {
        name  = "LLAMA_CPP_TAG"
        value = var.llama_cpp_tag
      }
      env {
        name  = "METEOR_NUM_THREADS"
        value = tostring(var.num_threads)
      }
      env {
        name  = "WORKER_OIDC_SA"
        value = google_service_account.tasks_invoker.email
      }
      # Not known at first apply — pass -var worker_url=... on the second apply
      # once the GKE worker Service/Ingress has an address (see infra/README.md).
      env {
        name  = "WORKER_URL"
        value = var.worker_url
      }
    }
  }
}
