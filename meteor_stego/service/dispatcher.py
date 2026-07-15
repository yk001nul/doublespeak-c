"""
Single-consumer job dispatcher — the enforcement point for concurrency=1.

The co-located llama-server is single-client (``--parallel 1``), so exactly one
encode/decode may run at a time. This dispatcher owns an asyncio queue and a
single-thread executor: submitted jobs are processed strictly one at a time, in
order. Additional throughput comes from running more worker processes, never
from widening this executor (see SERVICE_ARCHITECTURE.md "Deployment unit").
"""
import asyncio
import logging
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from typing import Callable

from .jobs import JobStore

log = logging.getLogger("meteor.service.dispatcher")


@dataclass
class _Task:
    job_id: str
    run: Callable[[], None]   # bound closure that performs the blocking job


class Dispatcher:
    def __init__(self, store: JobStore):
        self.store = store
        # The queue is created in start(), under the running event loop, so it
        # binds to the correct loop (a module-level Dispatcher would otherwise
        # bind to whatever loop existed at import time — wrong under TestClient,
        # which spins a fresh loop per test).
        self._queue: asyncio.Queue[_Task] | None = None
        # Both the queue and the executor are (re)created in start() so each
        # application lifespan gets fresh, correctly-bound instances.
        self._executor: ThreadPoolExecutor | None = None
        self._consumer: asyncio.Task | None = None

    def start(self) -> None:
        self._queue = asyncio.Queue()
        # max_workers=1 is the whole point: serialize every job.
        self._executor = ThreadPoolExecutor(max_workers=1,
                                            thread_name_prefix="meteor-worker")
        self._consumer = asyncio.create_task(self._run(), name="meteor-dispatcher")

    async def stop(self) -> None:
        if self._consumer:
            self._consumer.cancel()
            try:
                await self._consumer
            except asyncio.CancelledError:
                pass
        if self._executor:
            self._executor.shutdown(wait=False, cancel_futures=True)

    async def submit(self, job_id: str, run: Callable[[], None]) -> None:
        await self._queue.put(_Task(job_id=job_id, run=run))

    def depth(self) -> int:
        return self._queue.qsize()

    async def _run(self) -> None:
        loop = asyncio.get_running_loop()
        while True:
            task = await self._queue.get()
            self.store.mark_running(task.job_id)
            try:
                # Block this one job on the single worker thread; the event loop
                # stays free to serve status polls meanwhile.
                await loop.run_in_executor(self._executor, task.run)
            except Exception as exc:  # noqa: BLE001 - any failure → job failed
                log.exception("job %s failed", task.job_id)
                self.store.set_failed(task.job_id, f"{type(exc).__name__}: {exc}")
            finally:
                self._queue.task_done()
