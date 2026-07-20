# Interim cost optimization: scheduled daily scale-down of the expensive C2
# worker node pool during known-idle hours.
#
# The GKE worker nodes (~$150/mo each) are the dominant cost and sit idle most
# of the time (encode/decode is on-demand, minutes-long, rare). This turns the
# worker pool OFF on a daily cron and back ON in the morning, and pauses the
# Cloud Tasks queue in between so jobs submitted during the window wait (and
# drain in the morning) instead of failing delivery to an absent worker.
#
# Mechanism: a Cloud Run *job* running the public cloud-sdk image executes a
# gcloud sequence (gcloud waits for each GKE operation, so ordering is safe);
# two Cloud Scheduler crons invoke it with ACTION=down / ACTION=up.
#
# ENTIRELY OPT-IN: everything here is gated on var.cost_schedule_enabled
# (default false), so merging this changes nothing until you flip the flag on a
# terraform apply. Test the job by hand (ACTION=down -> confirm 0 nodes ->
# ACTION=up) before trusting the schedule — see infra/RUNBOOK.md.

locals {
  # gcloud sequence run inside the Cloud Run job. Reads ACTION + the cluster /
  # queue coordinates from env (set on the job below). gcloud commands are
  # synchronous — each blocks until its GKE operation completes — so the
  # disable-autoscaling -> resize -> (re)enable ordering is race-free.
  worker_scaler_script = <<-EOT
    set -euo pipefail
    echo "ACTION=$ACTION project=$PROJECT cluster=$CLUSTER pool=$POOL zone=$ZONE queue=$QUEUE"
    if [ "$ACTION" = "down" ]; then
      gcloud container clusters update "$CLUSTER" --node-pool "$POOL" --no-enable-autoscaling --zone "$ZONE" --project "$PROJECT" --quiet
      gcloud container clusters resize "$CLUSTER" --node-pool "$POOL" --num-nodes 0 --zone "$ZONE" --project "$PROJECT" --quiet
      gcloud tasks queues pause "$QUEUE" --location "$QLOC" --project "$PROJECT" --quiet
      echo "scaled DOWN: worker pool -> 0 nodes, queue paused"
    elif [ "$ACTION" = "up" ]; then
      gcloud container clusters resize "$CLUSTER" --node-pool "$POOL" --num-nodes "$MIN_NODES" --zone "$ZONE" --project "$PROJECT" --quiet
      gcloud container clusters update "$CLUSTER" --node-pool "$POOL" --enable-autoscaling --min-nodes "$MIN_NODES" --max-nodes "$MAX_NODES" --zone "$ZONE" --project "$PROJECT" --quiet
      gcloud tasks queues resume "$QUEUE" --location "$QLOC" --project "$PROJECT" --quiet
      echo "scaled UP: worker pool -> $MIN_NODES node(s), autoscaling restored, queue resumed"
    else
      echo "unknown ACTION=$ACTION (expected down|up)"; exit 1
    fi
  EOT
}

# Identity the Cloud Run job runs as: manages the worker node pool + the queue.
resource "google_service_account" "scaler" {
  count        = var.cost_schedule_enabled ? 1 : 0
  account_id   = "doublespeak-scaler"
  display_name = "doublespeak scheduled worker scaler (Cloud Run job)"
}

# Identity Cloud Scheduler uses to invoke the Cloud Run job.
resource "google_service_account" "scheduler" {
  count        = var.cost_schedule_enabled ? 1 : 0
  account_id   = "doublespeak-scheduler"
  display_name = "doublespeak Cloud Scheduler -> scaler job"
}

# Scaler: resize/update the worker node pool (cluster-management ops).
resource "google_project_iam_member" "scaler_gke" {
  count   = var.cost_schedule_enabled ? 1 : 0
  project = var.project_id
  role    = "roles/container.clusterAdmin"
  member  = "serviceAccount:${google_service_account.scaler[0].email}"
}

# Scaler: pause/resume ONLY the meteor-jobs queue (least privilege).
resource "google_cloud_tasks_queue_iam_member" "scaler_queue" {
  count    = var.cost_schedule_enabled ? 1 : 0
  location = google_cloud_tasks_queue.jobs.location
  name     = google_cloud_tasks_queue.jobs.name
  role     = "roles/cloudtasks.admin"
  member   = "serviceAccount:${google_service_account.scaler[0].email}"
}

# The scaler job itself — public cloud-sdk image, runs the gcloud sequence.
resource "google_cloud_run_v2_job" "worker_scaler" {
  count               = var.cost_schedule_enabled ? 1 : 0
  name                = "doublespeak-worker-scaler"
  location            = var.region
  deletion_protection = false

  template {
    template {
      service_account = google_service_account.scaler[0].email
      timeout         = "600s"
      max_retries     = 1

      containers {
        image   = "gcr.io/google.com/cloudsdktool/cloud-sdk:slim"
        command = ["/bin/bash", "-c"]
        args    = [local.worker_scaler_script]

        # ACTION defaults to "up" (the safe direction); each Scheduler invocation
        # overrides it per-run via the :run API containerOverrides.
        env {
          name  = "ACTION"
          value = "up"
        }
        env {
          name  = "PROJECT"
          value = var.project_id
        }
        env {
          name  = "CLUSTER"
          value = google_container_cluster.workers.name
        }
        env {
          name  = "POOL"
          value = google_container_node_pool.worker_pool.name
        }
        env {
          name  = "ZONE"
          value = var.zone
        }
        env {
          name  = "QLOC"
          value = google_cloud_tasks_queue.jobs.location
        }
        env {
          name  = "QUEUE"
          value = google_cloud_tasks_queue.jobs.name
        }
        env {
          name  = "MIN_NODES"
          value = tostring(var.worker_min_replicas)
        }
        env {
          name  = "MAX_NODES"
          value = tostring(var.worker_max_replicas)
        }
      }
    }
  }
}

# Let Cloud Scheduler's SA invoke (run) the scaler job.
resource "google_cloud_run_v2_job_iam_member" "scheduler_invokes_scaler" {
  count    = var.cost_schedule_enabled ? 1 : 0
  location = google_cloud_run_v2_job.worker_scaler[0].location
  name     = google_cloud_run_v2_job.worker_scaler[0].name
  role     = "roles/run.invoker"
  member   = "serviceAccount:${google_service_account.scheduler[0].email}"
}

# ── Cloud Scheduler crons ────────────────────────────────────────────────────
# POST to the Cloud Run Admin API :run endpoint, overriding ACTION per cron.
locals {
  scaler_run_uri = var.cost_schedule_enabled ? "https://${var.region}-run.googleapis.com/v2/projects/${var.project_id}/locations/${var.region}/jobs/${google_cloud_run_v2_job.worker_scaler[0].name}:run" : ""
}

resource "google_cloud_scheduler_job" "scale_down" {
  count     = var.cost_schedule_enabled ? 1 : 0
  name      = "doublespeak-worker-scale-down"
  region    = var.region
  schedule  = var.scale_down_cron
  time_zone = var.schedule_timezone

  http_target {
    http_method = "POST"
    uri         = local.scaler_run_uri
    headers     = { "Content-Type" = "application/json" }
    body = base64encode(jsonencode({
      overrides = { containerOverrides = [{ env = [{ name = "ACTION", value = "down" }] }] }
    }))
    oauth_token {
      service_account_email = google_service_account.scheduler[0].email
    }
  }
}

resource "google_cloud_scheduler_job" "scale_up" {
  count     = var.cost_schedule_enabled ? 1 : 0
  name      = "doublespeak-worker-scale-up"
  region    = var.region
  schedule  = var.scale_up_cron
  time_zone = var.schedule_timezone

  http_target {
    http_method = "POST"
    uri         = local.scaler_run_uri
    headers     = { "Content-Type" = "application/json" }
    body = base64encode(jsonencode({
      overrides = { containerOverrides = [{ env = [{ name = "ACTION", value = "up" }] }] }
    }))
    oauth_token {
      service_account_email = google_service_account.scheduler[0].email
    }
  }
}
