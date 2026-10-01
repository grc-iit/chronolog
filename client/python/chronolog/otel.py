import json
import math
import threading
import time
import uuid

from opentelemetry.sdk.trace.export import SpanExporter, SpanExportResult

from . import AlreadyExists, AppendResult, AppendSpec, Durability, Envelope, TimeReading, connect


def _attribute(value):
    return value if isinstance(value, str) else json.dumps(value, separators=(",", ":"), default=str)


class ChronologSpanExporter(SpanExporter):
    def __init__(self, catalog, player=None, *, chronicle="otel", default_story="spans",
                 identity=None, timeout=10.0):
        if not math.isfinite(timeout) or not 0 < timeout <= 300:
            raise ValueError("timeout must be finite and between 0 and 300 seconds")
        self.client = connect(catalog, player, timeout=timeout)
        self.chronicle, self.default_story, self.timeout = chronicle, default_story, timeout
        self.identity = identity or f"otel-{uuid.uuid4().hex}"
        self._writers = {}
        self._lock = threading.Lock()
        self._closed = False

    def _remaining(self, end):
        remaining = end - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("span export deadline")
        return remaining

    def _writer(self, story, end):
        if story in self._writers:
            return self._writers[story]
        try:
            self.client.create_chronicle(self.chronicle, timeout=self._remaining(end))
        except AlreadyExists:
            pass
        try:
            handle = self.client.create_story(self.chronicle, story, timeout=self._remaining(end))
        except AlreadyExists:
            stories = self.client.list_stories(self.chronicle, timeout=self._remaining(end))
            handle = next(s for s in stories if s.name == story and not s.tombstoned)
        writer = self.client.acquire(handle, self.identity, timeout=self._remaining(end))
        self._writers[story] = writer
        return writer

    def export(self, spans):
        end = time.monotonic() + self.timeout
        if not self._lock.acquire(timeout=self.timeout):
            return SpanExportResult.FAILURE
        try:
            if self._closed:
                return SpanExportResult.FAILURE
            batches = {}
            for span in spans:
                attrs = {key: _attribute(value) for key, value in (span.attributes or {}).items()}
                story = attrs.get("gen_ai.conversation.id") or self.default_story
                context = span.context
                payload = span.to_json(indent=None).encode("utf-8")
                envelope = Envelope(payload, "application/vnd.chronolog.otel-span+json", attrs,
                                    context.trace_id.to_bytes(16, "big"), context.span_id.to_bytes(8, "big"))
                batches.setdefault(story, []).append(AppendSpec(envelope, Durability.DURABLE,
                                                              TimeReading(span.end_time)))
                self._remaining(end)
            for story, items in batches.items():
                writer = self._writer(story, end)
                results = writer.append_batch(items, timeout=self._remaining(end))
                if len(results) != len(items) or any(not isinstance(r, AppendResult) or not r.acked for r in results):
                    return SpanExportResult.FAILURE
            return SpanExportResult.SUCCESS
        except Exception:
            return SpanExportResult.FAILURE
        finally:
            self._lock.release()

    def shutdown(self):
        self._lock.acquire()
        try:
            self._closed = True
            for writer in self._writers.values():
                try:
                    writer.release(timeout=self.timeout)
                except Exception:
                    pass
            self._writers.clear()
        finally:
            self._lock.release()

    def force_flush(self, timeout_millis=30000):
        return True
