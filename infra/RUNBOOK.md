# RUNBOOK — observability + emergency turn-down

Operational levers for the doublespeak stego service (Cloud Run front-end →
Cloud Tasks → GKE worker pool). Use this when traffic looks abusive, the service
is failing, or you need to stop work reaching the expensive GKE tier *now*.

All commands assume `gcloud`/`kubectl` are authenticated against project
`meteor-stego-1`, region `us-central1`, zone `us-central1-a`. Substitute your own
if different.

---

## 1. Observability — where the incoming requests are

Cloud Run and Cloud Tasks export request/queue metrics and request logs
automatically. The Terraform in `infra/terraform/monitoring.tf` adds a curated
dashboard + alert policies on top.

**Dashboard** (request rate by response class, p95 latency, queue depth, instance
count): `terraform output -raw dashboard_url`, or Cloud Console → Monitoring →
Dashboards → "doublespeak — front door".

**Live incoming requests** (Logs Explorer query — who is hitting the front door,
status, latency, source IP, user agent):

```
resource.type="cloud_run_revision"
resource.labels.service_name="doublespeak-frontend"
httpRequest.requestMethod!=""
```

**Top source IPs in the last hour** (gcloud):

```bash
gcloud logging read \
  'resource.type="cloud_run_revision" AND resource.labels.service_name="doublespeak-frontend" AND httpRequest.requestMethod!=""' \
  --freshness=1h --format='value(httpRequest.remoteIp)' | sort | uniq -c | sort -rn | head -20
```

**Alerts** (fire to `var.alert_email` if set): request spike
(`> alert_request_rate_threshold` req/s), 5xx surge, and job-queue backlog
(`> alert_queue_depth_threshold`). Set `-var alert_email=you@example.com` on apply
to receive them; without it the policies still exist and show in the console.

---

## 2. Turn-down levers (fastest → most disruptive)

The front-end is currently a **private** Cloud Run service (IAM-gated), so it is
not publicly reachable and cannot be flooded by anonymous traffic today. The
levers below matter most once it is made public.

### A. Pause the job queue — protect the expensive GKE tier (least disruptive)

Stops Cloud Tasks delivering to the workers. New jobs still get accepted + queued
(they drain when you resume), but no CPU-heavy encode/decode runs. This is the
first lever to reach for under load, because the GKE workers are the scarce,
expensive resource.

```bash
gcloud tasks queues pause  meteor-jobs --location us-central1     # stop delivery
gcloud tasks queues resume meteor-jobs --location us-central1     # restore
gcloud tasks queues purge  meteor-jobs --location us-central1     # drop the backlog (irreversible)
```

### B. Lower the front-end instance cap — bound cost/blast radius

Caps how far a flood can scale (and bill) the front door. Either edit
`var.frontend_max_instances` and re-apply, or hot-patch without Terraform:

```bash
gcloud run services update doublespeak-frontend --region us-central1 --max-instances=2
```

> Note: a raw `gcloud` change is Terraform drift — reconcile `frontend_max_instances`
> afterward, or the next `terraform apply` reverts it.

### C. Re-privatize the front-end — kill public access instantly

Only relevant if the service was made public (an `allUsers` invoker binding was
added for go-public). Removing it cuts off all anonymous traffic immediately;
authenticated callers with `roles/run.invoker` still work.

```bash
gcloud run services remove-iam-policy-binding doublespeak-frontend \
  --region us-central1 --member=allUsers --role=roles/run.invoker
```

### D. Scale the workers to zero — stop all encode/decode compute

Halts the GKE work entirely (and stops paying for the busy node). Jobs already
accepted stay `queued`/`running` in Firestore and resume when you scale back up.

```bash
kubectl -n doublespeak scale deployment doublespeak-worker --replicas=0    # stop
kubectl -n doublespeak scale deployment doublespeak-worker --replicas=1    # restore
```

### E. Full stop — take the front door offline

Most disruptive; rejects everything. Prefer A–C first.

```bash
# Redirect all traffic to nothing by removing the service's ability to serve:
gcloud run services update doublespeak-frontend --region us-central1 --no-traffic
# or delete outright (Terraform can recreate it on the next apply):
gcloud run services delete doublespeak-frontend --region us-central1 --quiet
```

---

## 3. After an incident

- Undo any raw `gcloud`/`kubectl` overrides, or reconcile them into Terraform
  (`frontend_max_instances`, IAM bindings) so the next `terraform apply` doesn't
  fight you. Overrides applied out-of-band are drift — the same trap as the
  worker OIDC settings.
- If you paused the queue, `resume` it (don't leave it paused — jobs silently pile
  up).
- Review the dashboard + Logs Explorer to confirm the source, and consider the
  next hardening step: **Cloud Armor** (external Application LB + serverless NEG)
  for real per-IP rate limiting and L7 DDoS Adaptive Protection — the proper
  public-facing defense, deferred until the go-public step.

---

## 4. Scheduled cost scaling (interim)

Turns the expensive C2 worker pool **off overnight** to cut the dominant cost.
Opt-in via Terraform `var.cost_schedule_enabled=true` (see `cost_schedule.tf`).
A Cloud Run job (`doublespeak-worker-scaler`) runs a `gcloud` sequence; two Cloud
Scheduler crons invoke it — `scale-down` (disable autoscaling → resize pool to 0
→ pause the queue) and `scale-up` (resize to `worker_min_replicas` → re-enable
autoscaling → resume the queue). During the down window the frontend still
accepts jobs; they queue (paused) and drain after scale-up. The morning's first
job pays a cold start (node provision + GGUF reload, a few min).

**Enable:** `terraform apply ... -var cost_schedule_enabled=true -var schedule_timezone=<your TZ> -var scale_down_cron="0 0 * * *" -var scale_up_cron="0 8 * * *"` (plus the standing vars, incl. `image_frontend` — verify it's non-empty!).

**Test by hand BEFORE trusting the schedule** — run the job directly and watch the pool:
```bash
gcloud run jobs execute doublespeak-worker-scaler --region us-central1 \
  --update-env-vars ACTION=down --wait
kubectl -n doublespeak get nodes            # expect the C2 node(s) to drain to 0
gcloud tasks queues describe meteor-jobs --location us-central1 --format='value(state)'  # PAUSED

gcloud run jobs execute doublespeak-worker-scaler --region us-central1 \
  --update-env-vars ACTION=up --wait
kubectl -n doublespeak get nodes            # C2 node returns; pod re-schedules + reloads model
```

**Manual override / kill the schedule:**
```bash
# Pause both crons (stops the automation without destroying it)
gcloud scheduler jobs pause doublespeak-worker-scale-down --location us-central1
gcloud scheduler jobs pause doublespeak-worker-scale-up   --location us-central1
# Force back up right now (if scaled down and you need it):
gcloud run jobs execute doublespeak-worker-scaler --region us-central1 --update-env-vars ACTION=up --wait
```
To remove entirely: `terraform apply ... -var cost_schedule_enabled=false` (tears down the scaler job, crons, and SAs). If you disable it while the pool is scaled **down**, run the `ACTION=up` execute (or a manual `gcloud container clusters resize ... --num-nodes 1` + re-enable autoscaling) first, or the pool stays at 0.

**Caveats:** only worthwhile if the idle window is real — a job submitted during the window waits until scale-up (not ideal for global/bursty traffic). Confirm the actual usage pattern from the monitoring dashboard before committing to a window.

## 5. Spot worker nodes (opt-in)

Moves the C2 worker pool onto Spot VMs (~60–70% off). Opt-in via Terraform `var.worker_use_spot=true`. Determinism-safe (same `machine_type` + `node_min_cpu_platform` as on-demand → byte-identical float math); preemption is absorbed by the idempotent Cloud Tasks re-drive — a killed job restarts on a replacement node.

**Enable / disable:**
```bash
terraform apply ... -var worker_use_spot=true    # move pool to Spot
terraform apply ... -var worker_use_spot=false   # back to on-demand
```

**⚠️ Toggling recreates the node pool** (`spot` is a pool-level attribute) — expect a brief worker outage plus a GGUF cold-reload on the new nodes. Do it in a maintenance window; drain/pause the queue first if a job is mid-flight (§4 scale-down, or `kubectl -n doublespeak scale deployment doublespeak-worker --replicas=0`).

**Caveats:** Spot capacity isn't guaranteed — under a c2 Spot shortage the autoscaler may fail to get a node and jobs wait. Each preemption costs a full job restart + GGUF reload, so only worthwhile once traffic tolerates the occasional retry. Watch the queue-backlog alert (§observability) after enabling. Composes with §4: scale-up brings the pool back as Spot nodes.
