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

Whether the front-end is publicly reachable depends on `var.frontend_public`.
While it is `false` the service is IAM-gated and cannot be flooded by anonymous
traffic; once it is `true` the external Application LB is the front door and
levers **0** and **C** below are the ones that shut it.

In public mode there are two independent gates in front of the workers, and it
is worth knowing which one to reach for: **Cloud Armor** bounds requests per
source IP, while the **API key quota** bounds jobs per caller. A flood of cheap
requests is an Armor problem; a caller burning real worker time is a quota
problem, and you revoke their key instead.

### 0. Slam the Cloud Armor default rule shut (public mode, fastest)

The quickest way to close a public service. Takes effect within a minute or two
and needs no redeploy, no Terraform, and no IAM change.

```bash
gcloud compute security-policies rules update 2147483647 \
  --security-policy doublespeak-frontend-policy --action deny-403
# restore:
gcloud compute security-policies rules update 2147483647 \
  --security-policy doublespeak-frontend-policy --action allow
```

To stay open for yourself while shutting out everyone else, set
`var.armor_allowed_ips = ["<your ip>/32"]` and re-apply — that is the same
staged-rollout state used before going public.

> Raw `gcloud` changes here are Terraform drift; reconcile them afterwards.

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

### B2. Revoke or throttle one caller — surgical, no outage

When the problem is a single API key rather than traffic volume, take that key
out instead of closing the door on everyone. Revocation takes up to
`API_KEY_CACHE_TTL_S` (60s) to propagate, because each Cloud Run instance caches
key records.

Run from the **repo root** (the `meteor_stego.service.gcp` module path has to
resolve), with `python3` — Cloud Shell is Linux, so the Windows `py -3` launcher
these commands used to be written with does not exist there:

```bash
cd ~/doublespeak-c
export GCP_PROJECT=meteor-stego-1
python3 -m meteor_stego.service.gcp.manage_keys list
python3 -m meteor_stego.service.gcp.manage_keys disable <key-hash>
```

On a fresh Cloud Shell the Firestore client may be missing
(`ModuleNotFoundError: google.cloud.firestore`):
```bash
python3 -m pip install --user -r meteor_stego/service/requirements-gcp.txt
```

Only the SHA-256 of each key is stored, so `list` shows hashes, not keys, and a
raw key is unrecoverable once minted — `create` prints it exactly once. If a key
is lost, mint a replacement and disable the old hash. To check whether a key you
hold is the registered one, compare `printf '%s' '<key>' | sha256sum` against
its hash — and send the **raw key** in requests, never the hash.

**Calling the API while the front-end is IAM-private.** Cloud Run needs
`Authorization: Bearer <Google identity token>` and forwards that header to the
app, so pass the API key in `X-API-Key`, which the app reads first:

```bash
curl -H "Authorization: Bearer $(gcloud auth print-identity-token)" \
     -H "X-API-Key: dsk_live_..." \
     -H "Content-Type: application/json" \
     -X POST "$(terraform -chdir=infra/terraform output -raw frontend_url)/v1/encode" -d '{...}'
```

`Authorization: Bearer <api-key>` also works, but only in public mode where no
identity token is competing for that header. In IAM-private mode it is the
identity token that occupies it, so `X-API-Key` is the form to use.

To find which key is responsible, the front-end logs `caller=<key-hash prefix>`
on every accepted job:

```bash
gcloud logging read \
  'resource.type="cloud_run_revision" AND textPayload:"accepted"' \
  --limit 100 --format='value(textPayload)'
```

Tighten limits globally instead of per-key by lowering `var.free_tier_quota_daily`
/ `var.admission_max_queued` and re-applying.

### C. Re-privatize the front-end — kill public access instantly

Removes the `allUsers` invoker binding, cutting off all anonymous traffic
immediately; callers holding `roles/run.invoker` still work. Note that in public
mode the Cloud Run ingress is also restricted to the load balancer, so lever 0
is usually the faster and less disruptive choice.

```bash
gcloud run services remove-iam-policy-binding doublespeak-frontend \
  --region us-central1 --member=allUsers --role=roles/run.invoker
```

The durable version is `-var frontend_public=false` on the next apply, which
reverts ingress to `INGRESS_TRAFFIC_ALL` and tears the load balancer down.

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

### 4a. Ad-hoc scale-down / scale-up (no schedule, no Terraform)

For a one-off idle stretch — a long local test run, a weekend, a pause between
deploys. This is the same gcloud sequence the scaler job runs, executed by hand,
so it works with `cost_schedule_enabled=false` (the default). **The Cloud Run
`doublespeak-worker-scaler` job only exists when that var is true**, so
`gcloud run jobs execute ...` above is not available unless you enabled §4.

Saves ~100% of worker node cost while down — strictly better than Spot (§5) for
an idle period, and with no pool recreation.

**Down:**
```bash
gcloud container clusters update doublespeak-workers --node-pool worker-pool \
  --no-enable-autoscaling --zone us-central1-a --quiet
gcloud container clusters resize doublespeak-workers --node-pool worker-pool \
  --num-nodes 0 --zone us-central1-a --quiet
gcloud tasks queues pause meteor-jobs --location us-central1 --quiet
```

**Up:**
```bash
gcloud container clusters resize doublespeak-workers --node-pool worker-pool \
  --num-nodes 1 --zone us-central1-a --quiet
gcloud container clusters update doublespeak-workers --node-pool worker-pool \
  --enable-autoscaling --min-nodes 1 --max-nodes 4 --zone us-central1-a --quiet
gcloud tasks queues resume meteor-jobs --location us-central1 --quiet
```

**Verify:**
```bash
kubectl -n doublespeak get nodes    # 0 when down; one C2 node when up
gcloud tasks queues describe meteor-jobs --location us-central1 --format='value(state)'
```

**Why autoscaling must be disabled first.** The worker Deployment is
`replicas: 1` (`k8s/worker-deployment.yaml`), so a pod is always pending and the
cluster autoscaler will immediately re-provision a node to satisfy it. Resizing
to 0 with autoscaling still enabled just bounces straight back. This is also why
setting `var.worker_min_replicas=0` on its own does **not** reduce idle cost — it
lowers the floor but does not stop the autoscaler from meeting pending demand.

**⚠️ This creates Terraform drift.** The `autoscaling` block is managed in
`main.tf`, so the next `terraform apply` re-enables autoscaling and scales the
pool back to `worker_min_replicas`. That is recoverable, not destructive, but do
not scale down by hand and then apply unrelated Terraform expecting the pool to
stay at 0. (§4's scheduled job creates the same drift by design.)

Match `--min-nodes`/`--max-nodes` on the way up to your current
`worker_min_replicas`/`worker_max_replicas` (defaults 1 and 4) or the next
`terraform plan` shows a spurious diff. Coming back up pays a cold start: node
provision plus a multi-GB GGUF reload (`--no-mmap`), a few minutes.

## 5. Spot worker nodes (opt-in)

Moves the C2 worker pool onto Spot VMs (~60–70% off). Opt-in via Terraform `var.worker_use_spot=true`. Determinism-safe (same `machine_type` + `node_min_cpu_platform` as on-demand → byte-identical float math). A preemption mid-run is recovered by the worker's **SIGTERM re-enqueue** (`worker_app.recover_inflight` resets the in-flight job to `queued` and re-pushes it), **not** by an automatic Cloud Tasks retry — the ack-fast worker already returned 200, so the task is gone from the queue. The re-run is deterministic (identical output). This needs the worker's Cloud Tasks enqueuer + act-as-invoker IAM and `TASKS_QUEUE`/`WORKER_URL` config, all added with the `worker_use_spot` var.

**Enable / disable:**
```bash
terraform apply ... -var worker_use_spot=true    # move pool to Spot
terraform apply ... -var worker_use_spot=false   # back to on-demand
```

**⚠️ Toggling recreates the node pool** (`spot` is a pool-level attribute) — expect a brief worker outage plus a GGUF cold-reload on the new nodes. Do it in a maintenance window; drain/pause the queue first if a job is mid-flight (§4 scale-down, or `kubectl -n doublespeak scale deployment doublespeak-worker --replicas=0`).

**Caveats:** Spot capacity isn't guaranteed — under a c2 Spot shortage the autoscaler may fail to get a node and jobs wait. Each preemption costs a full job restart + GGUF reload, so only worthwhile once traffic tolerates the occasional retry. Watch the queue-backlog alert (§observability) after enabling. Composes with §4: scale-up brings the pool back as Spot nodes.
