"""
CloudTasksQueue — enqueues a push task per submitted job.

A task carries only `{job_id, kind}`; the full request payload lives in the
Firestore job document. The task is an authenticated HTTP push to the worker's
`/internal/run`, using an OIDC token so only Cloud Tasks (as the configured
service account) can invoke the worker.

`max_concurrent_dispatches` on the queue (set in Terraform, = worker count) is
what serializes work to the single-client workers — this client just enqueues.

`google.cloud.tasks_v2` is imported lazily.
"""
import json

from . import settings


class CloudTasksQueue:
    def __init__(self, project=None, location=None, queue=None, worker_url=None,
                 oidc_sa=None):
        from google.cloud import tasks_v2  # lazy
        self._client = tasks_v2.CloudTasksClient()
        self._project = project or settings.GCP_PROJECT
        self._location = location or settings.TASKS_LOCATION
        self._queue = queue or settings.TASKS_QUEUE
        self._worker_url = (worker_url or settings.WORKER_URL).rstrip("/")
        self._oidc_sa = oidc_sa if oidc_sa is not None else settings.WORKER_OIDC_SA
        self._parent = self._client.queue_path(self._project, self._location, self._queue)

    def enqueue(self, job_id: str, kind: str, *, dedupe_name: bool = True) -> str:
        """Create a push task for (job_id, kind). Returns the created task name.

        By default `job_id` is the task name for at-least-once dedupe: Cloud Tasks
        rejects a duplicate task name within its dedup window, and the worker
        additionally dedupes on Firestore job status (jobs are idempotent).

        `dedupe_name=False` omits the name so Cloud Tasks auto-generates a unique
        one. The worker's shutdown recovery re-enqueues a job whose original,
        name=job_id task already *executed* (the ack-fast worker returned 200
        before it was preempted); Cloud Tasks refuses a reused task name for
        ~1 hour after execution, so the recovery push must not reuse it. Status-
        based dedupe on the worker still prevents a double-run."""
        from google.cloud import tasks_v2  # lazy
        url = f"{self._worker_url}/internal/run"
        http_request = {
            "http_method": tasks_v2.HttpMethod.POST,
            "url": url,
            "headers": {"Content-Type": "application/json"},
            "body": json.dumps({"job_id": job_id, "kind": kind}).encode("utf-8"),
        }
        if self._oidc_sa:
            http_request["oidc_token"] = {
                "service_account_email": self._oidc_sa,
                "audience": self._worker_url,
            }
        task = {"http_request": http_request}
        if dedupe_name:
            task["name"] = self._client.task_path(self._project, self._location,
                                                  self._queue, job_id)
        created = self._client.create_task(parent=self._parent, task=task)
        return created.name
