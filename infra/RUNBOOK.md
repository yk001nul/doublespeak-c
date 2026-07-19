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
