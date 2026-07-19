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
