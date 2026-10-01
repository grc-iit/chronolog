import asyncio
import json
import os
import sys
import time
import uuid

import chronolog as cl
import pytest

pytestmark = pytest.mark.skipif(not os.getenv("CHRONOLOG_TEST_VISOR"), reason="real stack required")


def test_otel_agent_tree_round_trip():
    from opentelemetry.sdk.trace import TracerProvider
    from opentelemetry.sdk.trace.export import SimpleSpanProcessor, SpanExportResult
    from chronolog.otel import ChronologSpanExporter

    chronicle = f"otel-{uuid.uuid4().hex}"
    exporter = ChronologSpanExporter(os.environ["CHRONOLOG_TEST_VISOR"], os.getenv("CHRONOLOG_TEST_PLAYER"),
                                    chronicle=chronicle, timeout=3)
    provider = TracerProvider()
    provider.add_span_processor(SimpleSpanProcessor(exporter))
    tracer = provider.get_tracer("chronolog-cx4")
    ids = []
    ends = []
    attributes = {"gen_ai.agent.id": "agent", "gen_ai.conversation.id": "conversation",
                  "gen_ai.operation.name": "invoke_agent", "gen_ai.tool.call.id": "call"}
    with tracer.start_as_current_span("agent", attributes=attributes) as root:
        with tracer.start_as_current_span("tool", attributes=attributes) as child:
            ids.append(child.get_span_context())
        ids.append(root.get_span_context())
    ends.extend([child.end_time, root.end_time])
    with tracer.start_as_current_span("default"):
        pass
    client = exporter.client
    stories = client.list_stories(chronicle, timeout=3)
    try:
        assert {s.name for s in stories} == {"conversation", "spans"}
        story = next(s for s in stories if s.name == "conversation")
        for _ in range(40):
            reader = client.read(story, end=cl.Hlc(time.time_ns()), timeout=3)
            events = list(reader)
            if len(events) == 2 and reader.completion.complete:
                break
            time.sleep(0.1)
        assert len(events) == 2 and reader.completion.complete
        for event, context, end_time in zip(events, ids, ends):
            assert event.physical.physical_ns == end_time
            assert event.envelope.trace_id == context.trace_id.to_bytes(16, "big")
            assert event.envelope.span_id == context.span_id.to_bytes(8, "big")
            assert event.envelope.attributes == attributes
            assert event.envelope.content_type == "application/vnd.chronolog.otel-span+json"
            payload = json.loads(event.payload)
            from datetime import datetime
            # JSON timestamps use microseconds; the physical field preserves nanoseconds.
            stamp = datetime.fromisoformat(payload["end_time"])
            assert abs(event.physical.physical_ns - int(stamp.timestamp() * 1e9)) < 2000
        provider.shutdown()
        assert exporter.export([]) == SpanExportResult.FAILURE
    finally:
        provider.shutdown()
        for story in stories:
            client.destroy_story(story, timeout=3)
        client.destroy_chronicle(chronicle, timeout=3)


def test_exporter_rejects_accepted_and_item_failures():
    from opentelemetry.sdk.trace import TracerProvider
    from opentelemetry.sdk.trace.export import SpanExportResult
    from chronolog.otel import ChronologSpanExporter

    exporter = ChronologSpanExporter(os.environ["CHRONOLOG_TEST_VISOR"], timeout=0.2)
    provider = TracerProvider()
    span = provider.get_tracer("failure").start_span("span")
    span.end()

    class Writer:
        result = cl.AppendResult(cl.EventId(), cl.Hlc(), cl.Durability.ACCEPTED)

        def append_batch(self, items, *, timeout):
            assert 0 < timeout <= 0.2
            return [self.result]

    writer = Writer()
    exporter._writers["spans"] = writer
    assert exporter.export([span]) == SpanExportResult.FAILURE
    writer.result = cl._error(14, "unavailable")
    assert exporter.export([span]) == SpanExportResult.FAILURE
    writer.result = cl.AppendResult(cl.EventId(), cl.Hlc(), cl.Durability.DURABLE)
    assert exporter.export([span]) == SpanExportResult.SUCCESS
    exporter._lock.acquire()
    started = time.monotonic()
    try:
        assert exporter.export([span]) == SpanExportResult.FAILURE
        assert time.monotonic() - started < 0.4
    finally:
        exporter._lock.release()
    exporter._writers.clear()
    exporter.shutdown()
    provider.shutdown()
