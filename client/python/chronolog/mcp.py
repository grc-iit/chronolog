import argparse
import asyncio
import base64
from contextlib import asynccontextmanager
from dataclasses import asdict
from functools import wraps
import json
import math
import os
from types import SimpleNamespace
import threading
import time
import uuid

from mcp.server.fastmcp import Context, FastMCP

from . import AlreadyExists, Cancelled, DeadlineExceeded, Durability, EventId, Hlc, connect


def _threaded(fn):
    @wraps(fn)
    async def run(*args, **kwargs):
        return await asyncio.to_thread(fn, *args, **kwargs)
    return run


def _json(value):
    return json.dumps(value, separators=(",", ":"))


def _event(event):
    envelope = event.envelope
    try:
        payload, encoded = envelope.payload.decode("utf-8"), False
    except UnicodeDecodeError:
        payload, encoded = base64.b64encode(envelope.payload).decode("ascii"), True
    return {"id": asdict(event.id), "hlc": asdict(event.hlc),
            "physical": asdict(event.physical), "durability": event.durability.name,
            "content": payload, "base64": encoded, "content_type": envelope.content_type,
            "attributes": envelope.attributes, "trace_id": envelope.trace_id.hex(),
            "span_id": envelope.span_id.hex()}


def _bound(value, maximum, name):
    if not 1 <= value <= maximum:
        raise ValueError(f"{name} must be between 1 and {maximum}")
    return value


class _State:
    def __init__(self, catalog, player, chronicle, identity, timeout):
        self.client = connect(catalog, player, timeout=timeout)
        self.chronicle, self.identity, self.timeout = chronicle, identity, timeout
        self.writer = None
        self.lock = threading.Lock()
        self.suffix = uuid.uuid4().hex

    def append(self, story, content, content_type, attributes, ctx):
        with self.lock:
            if self.writer is not None and self.writer.story_id != story:
                self.writer.release(timeout=self.timeout)
                self.writer = None
            if self.writer is None:
                if self.identity is None:
                    params = ctx.session.client_params
                    name = params.clientInfo.name if params else "mcp"
                    self.identity = f"{name}-{self.suffix}"
                self.writer = self.client.acquire(story, self.identity, timeout=self.timeout)
            attrs = dict(attributes or {})
            attrs["requestId"] = ctx.request_id
            result = self.writer.append(content.encode("utf-8"), content_type=content_type,
                                        attributes=attrs, durability=Durability.DURABLE, timeout=self.timeout)
            return _json({"event_id": asdict(result.event_id), "hlc": asdict(result.hlc),
                          "durability": result.durability.name, "acked": result.acked})

    def close(self):
        with self.lock:
            if self.writer is not None:
                self.writer.release(timeout=self.timeout)
                self.writer = None


def create_server(catalog, player=None, chronicle="chronolog", identity=None, timeout=10.0,
                  host="127.0.0.1", port=8000):
    if not math.isfinite(timeout) or not 0 < timeout <= 300:
        raise ValueError("timeout must be finite and between 0 and 300 seconds")

    shared_state = None
    active_sessions = 0
    session_lock = threading.Lock()

    @asynccontextmanager
    async def lifespan(server):
        nonlocal shared_state, active_sessions
        with session_lock:
            if shared_state is None:
                shared_state = _State(catalog, player, chronicle, identity, timeout)
            active_sessions += 1
            current = shared_state
        try:
            yield current
        finally:
            with session_lock:
                active_sessions -= 1
                if active_sessions == 0:
                    try:
                        current.close()
                    finally:
                        shared_state = None

    server = FastMCP("chronolog", lifespan=lifespan, host=host, port=port)

    def state(ctx):
        return ctx.request_context.lifespan_context

    @server.tool()
    @_threaded
    def chronolog_list_stories(ctx: Context, chronicle: str | None = None) -> str:
        """List stories in a chronicle."""
        s = state(ctx)
        return _json([asdict(story) for story in s.client.list_stories(chronicle or s.chronicle, timeout=s.timeout)])

    @server.tool()
    @_threaded
    def chronolog_create_story(story: str, ctx: Context, chronicle: str | None = None) -> str:
        """Create a story, creating its chronicle if needed."""
        s = state(ctx)
        name = chronicle or s.chronicle
        try:
            s.client.create_chronicle(name, timeout=s.timeout)
        except AlreadyExists:
            pass
        return _json(asdict(s.client.create_story(name, story, timeout=s.timeout)))

    @server.tool()
    @_threaded
    def chronolog_append(story: int, content: str, ctx: Context, content_type: str = "text/plain",
                         attributes: dict[str, str] | None = None) -> str:
        """Append content durably. acked is true only for DURABLE results."""
        return state(ctx).append(story, content, content_type, attributes, ctx)

    @server.tool()
    @_threaded
    def chronolog_read(story: int, ctx: Context, start_hlc: dict[str, int] | None = None,
                       end_hlc: dict[str, int] | None = None, limit: int = 1000) -> str:
        """Read a bounded HLC range with completion and an exclusive tail position."""
        _bound(limit, 10000, "limit")
        s = state(ctx)
        end = Hlc(**end_hlc) if end_hlc else Hlc(time.time_ns())
        events = []
        with s.client.read(story, Hlc(**start_hlc) if start_hlc else None, end, timeout=s.timeout) as reader:
            for event in reader:
                events.append(_event(event))
                if len(events) == limit:
                    break
            completion = asdict(reader.completion) if reader.completion else {
                "complete": False, "reason": "TRUNCATED", "frontier": asdict(event.hlc)}
            if reader.completion:
                completion["reason"] = reader.completion.reason.name
            return _json({"events": events, "completion": completion,
                          "continuation": asdict(reader.continuation) if reader.continuation else None,
                          "after": {"hlc": events[-1]["hlc"], "id": events[-1]["id"]} if events else None})

    @server.tool()
    @_threaded
    def chronolog_tail(story: int, ctx: Context, after: dict | None = None,
                       max_events: int = 100, timeout_s: float = 5.0) -> str:
        """Long poll exclusively after a position, bounded by count and timeout."""
        _bound(max_events, 10000, "max_events")
        if not math.isfinite(timeout_s) or not 0 < timeout_s <= 300:
            raise ValueError("timeout_s must be finite and between 0 and 300 seconds")
        position = SimpleNamespace(hlc=Hlc(**after["hlc"]), id=EventId(**after["id"])) if after else None
        s = state(ctx)
        events = []
        expired = threading.Event()
        with s.client.tail(story, position, timeout=timeout_s) as tail:
            def cancel():
                expired.set()
                tail.cancel()
            timer = threading.Timer(timeout_s, cancel)
            timer.start()
            try:
                for event in tail:
                    events.append(_event(event))
                    if len(events) == max_events:
                        break
            except (DeadlineExceeded, Cancelled):
                expired.set()
            finally:
                timer.cancel()
                timer.join()
        return _json({"events": events, "timed_out": expired.is_set(),
                      "after": {"hlc": events[-1]["hlc"], "id": events[-1]["id"]} if events else after})

    return server


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", default=os.getenv("CHRONOLOG_CATALOG", "127.0.0.1:50051"))
    parser.add_argument("--player", default=os.getenv("CHRONOLOG_PLAYER"))
    parser.add_argument("--chronicle", default=os.getenv("CHRONOLOG_CHRONICLE", "chronolog"))
    parser.add_argument("--identity", default=os.getenv("CHRONOLOG_WRITER_IDENTITY"))
    parser.add_argument("--timeout", type=float, default=10)
    parser.add_argument("--http", action="store_true")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8000)
    args = parser.parse_args()
    server = create_server(args.catalog, args.player, args.chronicle, args.identity, args.timeout, args.host, args.port)
    server.run(transport="streamable-http" if args.http else "stdio")


if __name__ == "__main__":
    main()
