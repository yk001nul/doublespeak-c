# Observability for the front door + turn-down signals.
#
# Cloud Run and Cloud Tasks already export request/queue metrics and request
# logs to Cloud Monitoring/Logging automatically; this file adds the curated
# layer on top: an email notification channel, alert policies that catch a
# request flood / error surge / job backlog, and a dashboard. The turn-down
# levers themselves (pause queue, re-privatize, scale to zero, lower the
# front-end instance cap) are manual — see infra/RUNBOOK.md.

# ── Notification channel (optional; set var.alert_email to enable) ───────────
resource "google_monitoring_notification_channel" "email" {
  count        = var.alert_email == "" ? 0 : 1
  display_name = "doublespeak alerts (email)"
  type         = "email"
  labels       = { email_address = var.alert_email }
}

# ── Alert: front-end request spike (possible abuse / DDoS) ───────────────────
resource "google_monitoring_alert_policy" "frontend_request_spike" {
  display_name = "doublespeak: front-end request spike"
  combiner     = "OR"
  documentation {
    content = "Front-end request rate exceeded ${var.alert_request_rate_threshold} req/s. If this is not expected traffic, consider the turn-down levers in infra/RUNBOOK.md (lower frontend_max_instances, pause the Cloud Tasks queue, or re-privatize the service)."
  }

  conditions {
    display_name = "front-end requests/sec > ${var.alert_request_rate_threshold}"
    condition_threshold {
      # Service name is static (google_cloud_run_v2_service.frontend.name), so
      # the policy can exist before the front-end is created on the 2nd apply.
      filter          = "resource.type = \"cloud_run_revision\" AND resource.labels.service_name = \"doublespeak-frontend\" AND metric.type = \"run.googleapis.com/request_count\""
      comparison      = "COMPARISON_GT"
      threshold_value = var.alert_request_rate_threshold
      duration        = "60s"
      trigger { count = 1 }
      aggregations {
        alignment_period     = "60s"
        per_series_aligner   = "ALIGN_RATE"
        cross_series_reducer = "REDUCE_SUM"
      }
    }
  }

  notification_channels = google_monitoring_notification_channel.email[*].id
}

# ── Alert: front-end 5xx surge (service failing under load or bug) ───────────
resource "google_monitoring_alert_policy" "frontend_5xx" {
  display_name = "doublespeak: front-end 5xx surge"
  combiner     = "OR"
  documentation {
    content = "Front-end is returning 5xx responses above ${var.alert_5xx_rate_threshold}/s — the service is failing (overload, crash loop, or a bad deploy). Check Cloud Run logs and instance count."
  }

  conditions {
    display_name = "front-end 5xx/sec > ${var.alert_5xx_rate_threshold}"
    condition_threshold {
      filter          = "resource.type = \"cloud_run_revision\" AND resource.labels.service_name = \"doublespeak-frontend\" AND metric.type = \"run.googleapis.com/request_count\" AND metric.labels.response_code_class = \"5xx\""
      comparison      = "COMPARISON_GT"
      threshold_value = var.alert_5xx_rate_threshold
      duration        = "60s"
      trigger { count = 1 }
      aggregations {
        alignment_period     = "60s"
        per_series_aligner   = "ALIGN_RATE"
        cross_series_reducer = "REDUCE_SUM"
      }
    }
  }

  notification_channels = google_monitoring_notification_channel.email[*].id
}

# ── Alert: Cloud Tasks backlog (flood, or workers stalled/undersized) ────────
resource "google_monitoring_alert_policy" "queue_backlog" {
  display_name = "doublespeak: job queue backlog"
  combiner     = "OR"
  documentation {
    content = "The meteor-jobs queue depth exceeded ${var.alert_queue_depth_threshold}. Either jobs are arriving faster than the single-slot workers can drain them (raise worker_max_replicas), or delivery/workers are stalled (check the worker Ingress + pod). To stop new work reaching the expensive GKE tier immediately: pause the queue (see infra/RUNBOOK.md)."
  }

  conditions {
    display_name = "meteor-jobs depth > ${var.alert_queue_depth_threshold}"
    condition_threshold {
      filter          = "resource.type = \"cloud_tasks_queue\" AND resource.labels.queue_id = \"${google_cloud_tasks_queue.jobs.name}\" AND metric.type = \"cloudtasks.googleapis.com/queue/depth\""
      comparison      = "COMPARISON_GT"
      threshold_value = var.alert_queue_depth_threshold
      duration        = "300s"
      trigger { count = 1 }
      aggregations {
        alignment_period   = "60s"
        per_series_aligner = "ALIGN_MEAN"
      }
    }
  }

  notification_channels = google_monitoring_notification_channel.email[*].id
}

# ── Alert: auth failures (someone guessing API keys) ─────────────────────────
# A public endpoint attracts credential-stuffing. Legitimate traffic produces
# almost no 401s — a client either holds a working key or it does not — so a
# sustained rate of them is someone probing, not users fumbling.
resource "google_monitoring_alert_policy" "frontend_401" {
  display_name = "doublespeak: API key auth failures"
  combiner     = "OR"
  documentation {
    content = "Front-end 401s above ${var.alert_401_rate_threshold}/s — likely API key guessing. Check the LB logs for the source IPs and, if it is one range, add a Cloud Armor deny rule (see infra/RUNBOOK.md). Individual keys can be revoked with manage_keys disable."
  }

  conditions {
    display_name = "front-end 401/sec > ${var.alert_401_rate_threshold}"
    condition_threshold {
      filter          = "resource.type = \"cloud_run_revision\" AND resource.labels.service_name = \"doublespeak-frontend\" AND metric.type = \"run.googleapis.com/request_count\" AND metric.labels.response_code = \"401\""
      comparison      = "COMPARISON_GT"
      threshold_value = var.alert_401_rate_threshold
      duration        = "300s"
      trigger { count = 1 }
      aggregations {
        alignment_period     = "60s"
        per_series_aligner   = "ALIGN_RATE"
        cross_series_reducer = "REDUCE_SUM"
      }
    }
  }

  notification_channels = google_monitoring_notification_channel.email[*].id
}

# ── Alert: sustained 429s (demand exceeds what one worker can serve) ─────────
# Unlike the others this is not necessarily an attack — it is the signal that
# real demand has outgrown the single-worker capacity, i.e. the moment to decide
# between raising quotas, adding a worker, or leaving callers throttled.
resource "google_monitoring_alert_policy" "frontend_429" {
  display_name = "doublespeak: sustained quota rejections"
  combiner     = "OR"
  documentation {
    content = "Front-end 429s above ${var.alert_429_rate_threshold}/s for 10 minutes. Either demand genuinely exceeds the single worker's ~300 jobs/day (consider worker capacity or per-key quota), or one caller is hammering a limit. Break it down by caller with the accepted-job log lines (caller=<key prefix>)."
  }

  conditions {
    display_name = "front-end 429/sec > ${var.alert_429_rate_threshold}"
    condition_threshold {
      filter          = "resource.type = \"cloud_run_revision\" AND resource.labels.service_name = \"doublespeak-frontend\" AND metric.type = \"run.googleapis.com/request_count\" AND metric.labels.response_code = \"429\""
      comparison      = "COMPARISON_GT"
      threshold_value = var.alert_429_rate_threshold
      duration        = "600s"
      trigger { count = 1 }
      aggregations {
        alignment_period     = "60s"
        per_series_aligner   = "ALIGN_RATE"
        cross_series_reducer = "REDUCE_SUM"
      }
    }
  }

  notification_channels = google_monitoring_notification_channel.email[*].id
}

# ── Budget alert ─────────────────────────────────────────────────────────────
# The backstop. Every other control here bounds a rate; this one bounds the
# actual bill, which is the number that ultimately matters when an endpoint is
# open to the internet. Opt-in because it needs roles/billing.admin on the
# billing account, which the rest of this config does not.
resource "google_billing_budget" "monthly" {
  count           = var.billing_account == "" ? 0 : 1
  billing_account = var.billing_account
  display_name    = "doublespeak monthly budget"

  budget_filter {
    projects = ["projects/${var.project_id}"]
  }

  amount {
    specified_amount {
      currency_code = "USD"
      units         = tostring(var.budget_amount_usd)
    }
  }

  # 50% and 90% are warnings; 100% means act. Forecasted spend fires early
  # enough to still do something about it.
  threshold_rules {
    threshold_percent = 0.5
  }
  threshold_rules {
    threshold_percent = 0.9
  }
  threshold_rules {
    threshold_percent = 1.0
  }
  threshold_rules {
    threshold_percent = 1.0
    spend_basis       = "FORECASTED_SPEND"
  }

  dynamic "all_updates_rule" {
    for_each = var.alert_email == "" ? [] : [1]
    content {
      monitoring_notification_channels = google_monitoring_notification_channel.email[*].id
      disable_default_iam_recipients   = false
    }
  }
}

# ── Dashboard: front door at a glance ────────────────────────────────────────
resource "google_monitoring_dashboard" "frontdoor" {
  dashboard_json = jsonencode({
    displayName = "doublespeak — front door"
    mosaicLayout = {
      columns = 48
      tiles = [
        {
          xPos = 0, yPos = 0, width = 24, height = 16
          widget = {
            title = "Front-end request rate by response class (req/s)"
            xyChart = {
              dataSets = [{
                plotType = "STACKED_BAR"
                timeSeriesQuery = {
                  timeSeriesFilter = {
                    filter = "resource.type=\"cloud_run_revision\" resource.labels.service_name=\"doublespeak-frontend\" metric.type=\"run.googleapis.com/request_count\""
                    aggregation = {
                      alignmentPeriod    = "60s"
                      perSeriesAligner   = "ALIGN_RATE"
                      crossSeriesReducer = "REDUCE_SUM"
                      groupByFields      = ["metric.label.response_code_class"]
                    }
                  }
                }
              }]
            }
          }
        },
        {
          xPos = 24, yPos = 0, width = 24, height = 16
          widget = {
            title = "Front-end request latency p95 (ms)"
            xyChart = {
              dataSets = [{
                plotType = "LINE"
                timeSeriesQuery = {
                  timeSeriesFilter = {
                    filter = "resource.type=\"cloud_run_revision\" resource.labels.service_name=\"doublespeak-frontend\" metric.type=\"run.googleapis.com/request_latencies\""
                    aggregation = {
                      alignmentPeriod    = "60s"
                      perSeriesAligner   = "ALIGN_PERCENTILE_95"
                      crossSeriesReducer = "REDUCE_MEAN"
                    }
                  }
                }
              }]
            }
          }
        },
        {
          xPos = 0, yPos = 16, width = 24, height = 16
          widget = {
            title = "Cloud Tasks meteor-jobs queue depth"
            xyChart = {
              dataSets = [{
                plotType = "LINE"
                timeSeriesQuery = {
                  timeSeriesFilter = {
                    filter = "resource.type=\"cloud_tasks_queue\" resource.labels.queue_id=\"meteor-jobs\" metric.type=\"cloudtasks.googleapis.com/queue/depth\""
                    aggregation = {
                      alignmentPeriod  = "60s"
                      perSeriesAligner = "ALIGN_MEAN"
                    }
                  }
                }
              }]
            }
          }
        },
        {
          xPos = 24, yPos = 16, width = 24, height = 16
          widget = {
            title = "Front-end active instances (vs. cap)"
            xyChart = {
              dataSets = [{
                plotType = "LINE"
                timeSeriesQuery = {
                  timeSeriesFilter = {
                    filter = "resource.type=\"cloud_run_revision\" resource.labels.service_name=\"doublespeak-frontend\" metric.type=\"run.googleapis.com/container/instance_count\""
                    aggregation = {
                      alignmentPeriod    = "60s"
                      perSeriesAligner   = "ALIGN_MEAN"
                      crossSeriesReducer = "REDUCE_SUM"
                    }
                  }
                }
              }]
            }
          }
        },
      ]
    }
  })
}
