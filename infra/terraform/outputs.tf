output "artifact_registry_repo" {
  value       = google_artifact_registry_repository.images.name
  description = "Push front-end/worker images here."
}

output "assets_bucket" {
  value       = google_storage_bucket.assets.name
  description = "Upload the pinned GGUF (var.gguf_object) here; large results land here too."
}

output "tasks_queue" {
  value       = google_cloud_tasks_queue.jobs.id
  description = "Cloud Tasks queue the front-end enqueues to."
}

output "frontend_service_account" {
  value = google_service_account.frontend.email
}

output "worker_service_account" {
  value       = google_service_account.worker.email
  description = "Annotate the k8s ServiceAccount with this (Workload Identity)."
}

output "tasks_invoker_service_account" {
  value       = google_service_account.tasks_invoker.email
  description = "OIDC identity Cloud Tasks uses to push to the worker (WORKER_OIDC_SA)."
}

output "worker_ingress_ip" {
  value       = google_compute_global_address.worker_ingress.address
  description = "Reserved static IP for the worker Ingress LB. The worker host is the sslip.io name for this IP (e.g. 35.201.65.159 -> 35-201-65-159.sslip.io); use https://<host> as -var worker_url and as the ConfigMap WORKER_OIDC_AUDIENCE (must match the managed cert + Ingress host)."
}

output "frontend_url" {
  value       = length(google_cloud_run_v2_service.frontend) > 0 ? google_cloud_run_v2_service.frontend[0].uri : ""
  description = "Cloud Run service URL (empty until image_frontend is set). NOTE: this attribute has historically been a stale alias that GFE-404s — derive the real one with `gcloud run services describe`. In public mode this URL stops answering anyway (ingress is restricted to the load balancer); use public_api_url instead."
}

output "frontend_ip" {
  value       = one(google_compute_global_address.frontend[*].address)
  description = "Reserved global IP of the public front-end load balancer (null unless frontend_public=true). Point a domain's A record here to move off the sslip.io host."
}

output "public_api_url" {
  value       = local.frontend_lb > 0 ? "https://${local.frontend_host}" : ""
  description = "The public API base URL. Empty unless frontend_public=true. The managed certificate takes 15-60 minutes to reach ACTIVE after the first apply — HTTPS fails until then."
}

output "dashboard_url" {
  value       = "https://console.cloud.google.com/monitoring/dashboards/builder/${reverse(split("/", google_monitoring_dashboard.frontdoor.id))[0]}?project=${var.project_id}"
  description = "Cloud Monitoring 'front door' dashboard (request rate/latency, queue depth, instance count)."
}
