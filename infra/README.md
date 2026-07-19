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

1. **Enable APIs**: cloudresourcemanager, iam, iamcredentials, run, cloudtasks,
   firestore, container, artifactregistry, secretmanager, monitoring, cloudbuild.
   (cloudresourcemanager + iam are required *before* the first apply — the
   `google_project_iam_member` resources read/modify the project IAM policy
   through Cloud Resource Manager; without it apply 403s after the GKE cluster
   is already created.)
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
     --substitutions=_REGION=us-central1,_REPO=doublespeak,SHORT_SHA=$(git rev-parse --short HEAD)
   ```
   `SHORT_SHA` is only auto-populated for trigger builds; a manual
   `gcloud builds submit` leaves it empty and the image tags come out invalid,
   so pass it explicitly as above.
5. **Deploy workers** (fill the `REPLACE_*` placeholders — project id, worker
   image ref, GGUF SHA, llama.cpp tag, and `REPLACE_WORKER_URL` = the reserved
   Ingress IP as `http://<terraform output worker_ingress_ip>` — via
   kustomize/envsubst):
   ```
   kubectl apply -f infra/k8s/configmap.yaml
   kubectl apply -f infra/k8s/worker-deployment.yaml
   kubectl apply -f infra/k8s/worker-service.yaml
   kubectl apply -f infra/k8s/worker-ingress.yaml   # external HTTP LB for Cloud Tasks
   kubectl apply -f infra/k8s/worker-hpa.yaml       # needs the Custom Metrics Adapter
   ```
   The Ingress binds the reserved static IP (`worker_ingress_ip`) and provisions
   an external HTTP LB — this takes a few minutes. Watch it come up:
   ```
   kubectl -n doublespeak get ingress doublespeak-worker -w
   ```
   `WORKER_OIDC_AUDIENCE` in the ConfigMap MUST equal the `worker_url` you pass
   in step 6 (same `http://<ip>`), or the worker rejects every push with 401/403.

   **Zone stockout:** if worker pods sit `Pending` with "no nodes available" and
   `gcloud compute instance-groups managed list-errors <MIG> --zone <zone>`
   shows `ZONE_RESOURCE_POOL_EXHAUSTED`, the zone is out of that machine type —
   a transient GCP capacity issue, not quota or config. Re-apply Terraform with
   a different `-var zone=...` (and consider `-var worker_machine_type=
   e2-standard-4`; E2 is less stockout-prone than N2). The cluster is zonal, so
   changing the zone **recreates the cluster** — harmless before anything runs
   on it. Afterwards re-run `gcloud container clusters get-credentials` with the
   new `--zone` and redo the `kubectl apply` steps above.
6. **Wire the worker URL back**: the worker's public address is the reserved
   Ingress IP. Re-apply Terraform with both `-var image_frontend=<ref>` and
   `-var worker_url=http://$(terraform output -raw worker_ingress_ip)` set, so the
   Cloud Run front-end comes up pointing at the queue + worker. (`worker_url`
   becomes the front-end's `WORKER_URL` env — the front-end refuses to start
   without it — and Cloud Tasks signs each push's OIDC token with it as the
   audience, which the worker validates against `WORKER_OIDC_AUDIENCE`.) These two
   values must be byte-identical.
7. **Verify end-to-end**: `POST /v1/encode` on the front-end URL (returns 202 +
   `job_id`), then poll `GET /v1/jobs/{job_id}` — it should walk `queued →
   running → done` with the recovered covertext. A job stuck in `queued` means
   Cloud Tasks can't deliver (check the Ingress has an ADDRESS and the worker
   pod is Ready); repeated `running → queued` retries with 401/403 in the worker
   logs mean the OIDC audience/SA don't match the ConfigMap.

## Known follow-ups (Phase 4 "Harden")

- ~~OIDC validation at the worker `/internal/run`.~~ **Done** — the worker
  verifies the Cloud Tasks OIDC token (`WORKER_REQUIRE_OIDC`, `gcp/oidc.py`):
  signature + audience (`WORKER_OIDC_AUDIENCE`) + caller identity
  (`WORKER_OIDC_SA`). The endpoint is public (behind the HTTP LB) but
  authenticated per-request.
- **TLS on the worker LB**: currently HTTP only (`worker_url=http://<ip>`); the
  OIDC token rides in plaintext. Add a domain + `ManagedCertificate` for HTTPS.
- ~~Observability dashboards.~~ **Done** — `terraform/monitoring.tf` adds a
  "front door" dashboard (request rate by response class, p95 latency, queue
  depth, instance count), alert policies (request spike / 5xx surge / queue
  backlog → `var.alert_email`), and a Cloud Run `max_instance_count` cap
  (`var.frontend_max_instances`). Emergency turn-down levers (pause queue,
  re-privatize, scale workers to 0) are documented in `infra/RUNBOOK.md`.
- Front-end auth/quota via API Gateway; webhooks (`callback_url`); per-identity
  Secret Manager stego keys.
- **Cloud Armor** (external Application LB + serverless NEG in front of Cloud Run)
  for per-IP rate limiting + L7 DDoS Adaptive Protection — the real public-facing
  defense, deferred to the go-public step.
- The queue-depth HPA needs the Custom Metrics Stackdriver Adapter installed.
