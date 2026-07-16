terraform {
  required_version = ">= 1.6"

  required_providers {
    google = {
      source  = "hashicorp/google"
      version = ">= 5.0, < 7.0"
    }
  }

  # Configure a GCS backend for shared state before real use, e.g.:
  # backend "gcs" {
  #   bucket = "YOUR-TF-STATE-BUCKET"
  #   prefix = "doublespeak/service"
  # }
}

provider "google" {
  project = var.project_id
  region  = var.region
}
