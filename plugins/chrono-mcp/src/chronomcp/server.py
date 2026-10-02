import argparse
import asyncio
from contextlib import asynccontextmanager
from dataclasses import asdict
from functools import wraps
import json
import math
import os
from types import SimpleNamespace
import threading
import uuid

from mcp.server.mcpserver import Context, MCPServer

from .reader import _json, _bound, _event, read_story

from chronolog import AlreadyExists, Cancelled, DeadlineExceeded, Durability, EventId, Hlc, connect


def _threaded(fn):
    @wraps(fn)
    async def run(*args, **kwargs):
        return await asyncio.to_thread(fn, *args, **kwargs)
    return run


class _State:
    def __init__(self, catalog, player, chronicle, identity, timeout):
        self.client = connect(catalog, player, timeout=timeout)
        self.chronicle, self.identity, self.timeout = chronicle, identity, timeout
        self.writer = None
        self.lock = threading.RLock()
        self.active_story = None
        self.suffix = uuid.uuid4().hex

    def create_story(self, chronicle, name):
        try:
            self.client.create_chronicle(chronicle, timeout=self.timeout)
        except AlreadyExists:
            pass
        try:
            return self.client.create_story(chronicle, name, timeout=self.timeout)
        except AlreadyExists:
            return self.find_story(chronicle, name)

    def find_story(self, chronicle, name):
        stories = self.client.list_stories(chronicle, timeout=self.timeout)
        for story in stories:
            if story.name == name and not story.tombstoned:
                return story
        raise ValueError(f"Story {chronicle}/{name} does not exist")

    def acquire(self, story, ctx):
        if self.writer is not None and self.writer.story_id != story:
            self.writer.release(timeout=self.timeout)
            self.writer = None
        if self.writer is None:
            if self.identity is None:
                params = ctx.session.client_params
                name = params.clientInfo.name if params else "mcp"
                self.identity = f"{name}-{self.suffix}"
            self.writer = self.client.acquire(story, self.identity, timeout=self.timeout)
        return self.writer

    def append(self, story, content, content_type, attributes, ctx, trace_id=None, span_id=None):
        with self.lock:
            writer = self.acquire(story, ctx)
            attrs = dict(attributes or {})
            attrs["requestId"] = ctx.request_id
            result = writer.append(content.encode("utf-8"), content_type=content_type,
                                   attributes=attrs, durability=Durability.DURABLE, timeout=self.timeout,
                                   trace_id=bytes.fromhex(trace_id) if trace_id else None,
                                   span_id=bytes.fromhex(span_id) if span_id else None)
            return _json({"event_id": asdict(result.event_id), "hlc": asdict(result.hlc),
                          "durability": result.durability.name, "acked": result.acked})

    def close(self):
        with self.lock:
            if self.writer is not None:
                self.writer.release(timeout=self.timeout)
                self.writer = None
            self.active_story = None


def create_server(catalog, player=None, chronicle="chronolog", identity=None, timeout=10.0,
                  host="127.0.0.1", port=8000):
    if not math.isfinite(timeout) or not 0 < timeout <= 300:
        raise ValueError("timeout must be finite and between 0 and 300 seconds")

    shared_state = None

    @asynccontextmanager
    async def lifespan(server):
        nonlocal shared_state
        current = shared_state = _State(catalog, player, chronicle, identity, timeout)
        try:
            yield current
        finally:
            shared_state = None
            current.close()

    server = MCPServer("chronolog", lifespan=lifespan)

    def state(ctx):
        return ctx.request_context.lifespan_context

    @server.tool()
    @_threaded
    def list_stories(ctx: Context, chronicle: str | None = None) -> str:
        """List stories in a chronicle."""
        s = state(ctx)
        return _json([asdict(story) for story in s.client.list_stories(chronicle or s.chronicle, timeout=s.timeout)])

    @server.tool()
    @_threaded
    def create_story(story: str, ctx: Context, chronicle: str | None = None) -> str:
        """Create a story, creating its chronicle if needed."""
        s = state(ctx)
        name = chronicle or s.chronicle
        return _json(asdict(s.create_story(name, story)))

    @server.tool()
    @_threaded
    def append(story: int, content: str, ctx: Context, content_type: str = "text/plain",
               attributes: dict[str, str] | None = None,
               trace_id: str | None = None, span_id: str | None = None) -> str:
        """Append content durably. acked is true only for DURABLE results."""
        return state(ctx).append(story, content, content_type, attributes, ctx, trace_id, span_id)

    @server.tool()
    @_threaded
    def read(story: int, ctx: Context, start_hlc: dict[str, int] | None = None,
             end_hlc: dict[str, int] | None = None, limit: int = 1000) -> str:
        """Read an HLC range with Replay Completion. A count limit returns limited=true and completion=null."""
        s = state(ctx)
        return _json(read_story(s.client, story, start_hlc, end_hlc, limit, s.timeout))

    @server.tool()
    @_threaded
    def tail(story: int, ctx: Context, after: dict | None = None,
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

    @server.tool()
    @_threaded
    def start_chronolog(ctx: Context, chronicle_name: str | None = None,
                        story_name: str = "conversation") -> str:
        """Start or resume a conversation story and acquire its writer."""
        s = state(ctx)
        with s.lock:
            story = s.create_story(chronicle_name or s.chronicle, story_name)
            writer = s.acquire(story.id, ctx)
            s.active_story = story
            return _json({"status": "started", "story": asdict(story), "writer_id": writer.writer_id})

    @server.tool()
    @_threaded
    def record_interaction(user_message: str, assistant_message: str, ctx: Context,
                           attributes: dict[str, str] | None = None,
                           trace_id: str | None = None, span_id: str | None = None) -> str:
        """Record a structured user and assistant interaction with DURABLE acknowledgement."""
        s = state(ctx)
        with s.lock:
            if s.active_story is None:
                raise ValueError("No active ChronoLog session. Call start_chronolog first.")
            attrs = {"gen_ai.operation.name": "chat",
                     "gen_ai.conversation.id": f"{s.active_story.chronicle}/{s.active_story.name}"}
            attrs.update(attributes or {})
            content = _json({"user_message": user_message, "assistant_message": assistant_message})
            return s.append(s.active_story.id, content, "application/vnd.chronolog.interaction+json",
                            attrs, ctx, trace_id, span_id)

    @server.tool()
    @_threaded
    def retrieve_interaction(ctx: Context, chronicle_name: str | None = None,
                             story_name: str | None = None, start_hlc: dict[str, int] | None = None,
                   end_hlc: dict[str, int] | None = None, limit: int = 1000) -> str:
        """Retrieve conversation events and Replay Completion. A count limit returns limited=true and completion=null."""
        s = state(ctx)
        with s.lock:
            active = s.active_story
        chronicle = chronicle_name or (active.chronicle if active else s.chronicle)
        name = story_name or (active.name if active else "conversation")
        story = s.find_story(chronicle, name)
        return _json(read_story(s.client, story.id, start_hlc, end_hlc, limit, s.timeout))

    @server.tool()
    @_threaded
    def stop_chronolog(ctx: Context) -> str:
        """Release the process writer and end the active conversation session."""
        s = state(ctx)
        s.close()
        return _json({"status": "stopped"})

    @server.resource("chronolog://status")
    @_threaded
    def chronolog_status() -> str:
        """Report actual conversation and writer state."""
        s = shared_state
        if s is None:
            return _json({"service": "chronolog", "status": "idle", "chronicle": chronicle,
                          "story": None, "writer_id": None})
        with s.lock:
            return _json({"service": "chronolog", "status": "active" if s.writer else "idle",
                          "chronicle": s.chronicle,
                          "story": asdict(s.active_story) if s.active_story else None,
                          "writer_id": s.writer.writer_id if s.writer else None})

    @server.prompt()
    def logging_workflow(time_range: str = "an HLC range") -> str:
        """Guide conversation logging and analysis with explicit completeness."""
        return (f"Analyze ChronoLog interactions over {time_range}. Use start_chronolog and record_interaction "
                "to capture conversations; retrieve_interaction or read returns events and Replay Completion. "
                "Report complete, reason and frontier before drawing conclusions. Use tail to follow new events, "
                "and stop_chronolog to release the writer. acked means DURABLE only.")

    return server, dict(host=host, port=port)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", default=os.getenv("CHRONOLOG_CATALOG", "127.0.0.1:50051"))
    parser.add_argument("--player", default=os.getenv("CHRONOLOG_PLAYER"))
    parser.add_argument("--chronicle", default=os.getenv("CHRONOLOG_CHRONICLE", "chronolog"))
    parser.add_argument("--identity", default=os.getenv("CHRONOLOG_WRITER_IDENTITY"))
    parser.add_argument("--timeout", type=float, default=10)
    parser.add_argument("--http", action="store_true")
    parser.add_argument("--transport", choices=["stdio", "http"], default=os.getenv("MCP_TRANSPORT", "stdio"))
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8000)
    args = parser.parse_args()
    server, address = create_server(args.catalog, args.player, args.chronicle, args.identity, args.timeout,
                                    args.host, args.port)
    if args.http or args.transport == "http":
        server.run(transport="streamable-http", **address)
    else:
        server.run(transport="stdio")


if __name__ == "__main__":
    main()
