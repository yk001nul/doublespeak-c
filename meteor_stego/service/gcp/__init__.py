"""GCP-mode components: Cloud Run front-end + GKE worker, connected by Cloud
Tasks, with Firestore as the job source of truth and GCS for the model and large
payloads. All google-cloud client libraries are lazy-imported so the local MVP
and fake-backed unit tests do not require them installed."""
