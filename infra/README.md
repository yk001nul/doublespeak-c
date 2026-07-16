# infra/ — GCP deployment for the doublespeak stego service (Phase 2)

Apply-later infrastructure-as-code for the topology in `SERVICE_ARCHITECTURE.md`
"Phase 2 — GCP topology": a **Cloud Run front-end** enqueues to **Cloud Tasks**,
which pushes to a **GKE worker pool**, with **Firestore** as the job source of
truth and **GCS** for the pinned model and large payloads.

Nothing here has been `terraform apply`-ed or deployed — it is written to be
applied when a GCP project is available. The application code it deploys lives in
`meteor_stego/service/` (front-end: `gcp/frontend_app.py`, worker:
`gcp/worker_app.py`).

## Layout

```
infra/
  terraform/     GCP resources (Firestore, Cloud Tasks, GCS, Artifact Registry,
                 Secret Manager, Cloud Run front-end, GKE cluster + node pool, IAM)
  docker/        Dockerfile.frontend (slim) and Dockerfile.worker (multi-stage:
                 builds meteor.so + llama.cpp, GGUF pulled from GCS at runtime)
  cloudbuild/    cloudbuild.yaml — builds + pushes both images to Artifact Registry
  k8s/           worker Deployment (+ llama-server sidecar), Service, queue-depth
                 HPA, determinism ConfigMap
```

## Determinism is a served contract

Every worker must be **byte-identical**: same GGUF (SHA-256 verified on boot —
`gcp/gcs.ensure_model` aborts the pod on mismatch), same `METEOR_NUM_THREADS`,
same pinned llama.cpp tag, same sampling flags. These are set once in
`k8s/configmap.yaml` and the Cloud Run env (Terraform), and published to clients
via `GET /v1/model-info`. A single mismatched worker silently corrupts messages,
because any worker can pick up any job. Raising `num_threads` re-validates
`styled_encode`/`roundtrip` and bumps the served protocol version.

## Apply order (once you have a project)

1. **Enable APIs**: run, cloudtasks, firestore, container, artifactregistry,
   secretmanager, monitoring, cloudbuild.
2. **Terraform** (creates registry, bucket, Firestore, queue, GKE, IAM):
   ```
   cd infra/terraform
   terraform init
   terraform apply -var project_id=YOUR_PROJECT \
     -var gguf_sha256=<sha256 of the GGUF> -var llama_cpp_tag=<pinned tag>
   ```
   Leave `image_frontend` and `worker_url` empty on the first apply — the Cloud
   Run front-end is gated on `image_frontend` (`count = image_frontend == "" ? 0
   : 1`) and isn't created yet, and `worker_url` isn't known until the GKE worker
   Service exists (both are set on the second apply, step 6).

   **Networking:** the GKE cluster defaults to the project's auto-created
   `default` VPC. Projects created with the default-network org policy disabled
   have no `default` network and the apply fails with a network-not-found error —
   check with `gcloud compute networks list` and, if needed, pass
   `-var network=<vpc>` (and `-var subnetwork=<subnet in var.region>` for a
   custom-mode network; leave it empty to let GKE auto-select on an auto-mode
   network).
3. **Upload the model** to the assets bucket at `var.gguf_object`:
   ```
   gsutil cp Phi-3.5-mini-instruct-Q4_K_M.gguf \
     gs://$(terraform output -raw assets_bucket)/models/
   ```
4. **Build + push images**:
   ```
   gcloud builds submit --config infra/cloudbuild/cloudbuild.yaml \
     --substitutions=_REGION=us-central1,_REPO=doublespeak
   ```
5. **Deploy workers** (fill the `REPLACE_*` placeholders — project id, worker
   image ref, GGUF SHA, llama.cpp tag — via kustomize/envsubst):
   ```
   kubectl apply -f infra/k8s/configmap.yaml
   kubectl apply -f infra/k8s/worker-deployment.yaml
   kubectl apply -f infra/k8s/worker-service.yaml
   kubectl apply -f infra/k8s/worker-hpa.yaml   # needs the Custom Metrics Adapter
   ```
6. **Wire the worker URL back**: get the worker Service's internal address, then
   re-apply Terraform with both `-var image_frontend=<ref>` and
   `-var worker_url=http://<worker-address>` set, so the Cloud Run front-end
   comes up pointing at the queue + worker. (`worker_url` becomes the front-end's
   `WORKER_URL` env — the front-end refuses to start without it.) The worker
   Service is `ClusterIP`, so for Cloud Tasks to reach it from outside the cluster
   you must expose it via an internal LB / Ingress (Phase 4 hardening).

## Known follow-ups (Phase 4 "Harden")

- The worker `/internal/run` must sit behind an OIDC-validating proxy / IAP so
  only the Cloud Tasks invoker SA can reach it (the queue attaches an OIDC token;
  the endpoint should verify it). The manifests leave it ClusterIP with a note.
- Front-end auth/quota via API Gateway; webhooks (`callback_url`); observability
  dashboards; per-identity Secret Manager stego keys.
- The queue-depth HPA needs the Custom Metrics Stackdriver Adapter installed.
