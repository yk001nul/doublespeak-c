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

output "frontend_url" {
  value       = length(google_cloud_run_v2_service.frontend) > 0 ? google_cloud_run_v2_service.frontend[0].uri : ""
  description = "Public front-end URL (empty until image_frontend is set)."
}
