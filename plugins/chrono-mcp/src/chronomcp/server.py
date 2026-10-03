"""chrono-mcp: twelve instance and Context tools over the native Context API (RFC-F section 7)."""
import argparse
import asyncio
import base64
from contextlib import asynccontextmanager
from dataclasses import replace
from datetime import datetime, timezone
from functools import wraps
import math
import os
import re
import secrets
import signal
import socket
import threading
import time
import uuid

from mcp.server.mcpserver import MCPServer
from mcp.server.mcpserver.exceptions import ToolError

import chronolog as cl

from .instances import Instances

from .store import (CheckpointStore, Launcher, Unavailable, compact, hlc_json, position_of, position_values, token,
                    untoken)

LABEL = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}")
DEFAULT_EVENTS, DEFAULT_JSON = 50, 24 << 10
OMITTED = ["durability", "physical", "attributes", "trace_id", "span_id"]


def _threaded(fn):
    @wraps(fn)
    async def run(*args, **kwargs):
        try:
            return await asyncio.to_thread(fn, *args, **kwargs)
        except Unavailable as error:
            return compact(_unavailable(error))
        except (ValueError, TypeError, OSError, cl.Error) as error:
            raise ToolError(f"{type(error).__name__}: {error}") from error
    return run


def _bound(value, low, high, name):
    if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
        raise ValueError(f"{name} must be an integer between {low} and {high}")
    return value


def chronolog_home():
    return os.getenv("CHRONOLOG_HOME") or os.path.join(
        os.getenv("XDG_STATE_HOME") or os.path.expanduser("~/.local/state"), "chronolog")


def acceptance_bound(value, name):
    if isinstance(value, int) and not isinstance(value, bool):
        ns = value
    elif isinstance(value, str):
        match = re.fullmatch(
            r"(\d{4}-\d{2}-\d{2})[Tt](\d{2}:\d{2}:\d{2})(?:\.(\d{1,9}))?([Zz]|[+-]\d{2}:\d{2})", value)
        if match is None:
            raise ValueError(f"{name} must be int64 nanoseconds or RFC 3339 with a timezone and at most 9 fractional digits")
        date, clock, fraction, offset = match.groups()
        if offset.lower() != "z" and (int(offset[1:3]) > 23 or int(offset[4:]) > 59):
            raise ValueError(f"{name} has an invalid timezone offset")
        instant = datetime.fromisoformat(f"{date}T{clock}{'+00:00' if offset.lower() == 'z' else offset}")
        delta = instant.astimezone(timezone.utc) - datetime(1970, 1, 1, tzinfo=timezone.utc)
        ns = (delta.days * 86400 + delta.seconds) * 1_000_000_000 + int((fraction or "").ljust(9, "0"))
    else:
        raise ValueError(f"{name} must be int64 nanoseconds or RFC 3339")
    _bound(ns, -(2**63), 2**63 - 1, name)
    return cl.Hlc(ns, 0)


def _status(s):
    if s is None:
        return None
    out = {"code": s.code.name, "message": s.message}
    if s.rejection is not cl.AppendRejection.UNSPECIFIED:
        out["rejection"] = s.rejection.name
    if s.acquire_refusal is not None:
        r = s.acquire_refusal
        out["acquire_refusal"] = {"reason": r.refusal_reason.name,
                                  "current_incarnation": None if r.current_incarnation is None
                                  else str(r.current_incarnation)}
    return out


def _completion(c):
    if c is None:
        return None
    return {"complete": c.complete, "reason": c.reason.name, "frontier": hlc_json(c.frontier),
            "laggards": [{"writer_id": str(f.writer_id), "incarnation": str(f.incarnation),
                          "frontier": hlc_json(f.frontier)} for f in c.laggards]}


def _range(r):
    return None if r is None else {"start": hlc_json(r.start), "end": hlc_json(r.end)}


def _stamp(w):
    return None if w is None else {"writer_id": str(w.writer_id), "incarnation": str(w.incarnation)}


def _at(story_id, h):
    return token("a1", [str(story_id), str(h.physical_ns), h.logical])


def _follow_token(p):
    return token("f1", position_values(p))


def _event(e, view):
    try:
        content = {"encoding": "utf8", "data": e.envelope.payload.decode("utf-8")}
    except UnicodeDecodeError:
        content = {"encoding": "base64", "data": base64.b64encode(e.envelope.payload).decode("ascii")}
    out = {"id": {"story_id": str(e.id.story_id), "writer_id": str(e.id.writer_id),
                  "incarnation": str(e.id.incarnation), "sequence": str(e.id.sequence)},
           "hlc": hlc_json(e.hlc), "at": _at(e.id.story_id, e.hlc), "content_type": e.envelope.content_type,
           "content": content}
    if view == "full":
        out.update({"follow_token": _follow_token(cl.Position(e.hlc, e.id)), "durability": e.durability.name,
                    "physical": {"physical_ns": str(e.physical.physical_ns),
                                 "uncertainty_ns": None if e.physical.uncertainty_ns is None
                                 else str(e.physical.uncertainty_ns), "status": e.physical.status},
                    "attributes": e.envelope.attributes, "trace_id": e.envelope.trace_id.hex(),
                    "span_id": e.envelope.span_id.hex()})
    return out


def _result(verdict, answer_complete, has_more=False, next_cursor=None, **rest):
    return {"verdict": verdict, "answer_complete": answer_complete, "has_more": has_more,
            "next_cursor": next_cursor, **rest}


def _size(value):
    return len(compact(value).encode())


class _Session:
    def __init__(self, handle, label, ref, access, native):
        self.handle, self.label, self.ref, self.access, self.native = handle, label, ref, access, native
        self.persist_required = False
        self.closed_verdict = None
        self.last_used = time.monotonic()

    @property
    def writable(self):
        return self.access is cl.Access.READ_WRITE


class _State:
    def __init__(self, args):
        self.args = args
        self.contexts = self.launcher = self.store = None
        self.instances = Instances(args)
        self.sessions = {}
        self.lock = threading.RLock()
        self.active_calls = 0
        self.stop = threading.Event()
        self.idle = None
        if args.idle_close_s > 0:
            self.idle = threading.Thread(target=self._idle_loop, daemon=True)
            self.idle.start()

        record = self.instances.startup()
        if record is not None:
            try:
                self.bind(record)
            except BaseException:
                if self.launcher is not None:
                    self.launcher.release()
                self.instances.detach()
                self.stop.set()
                raise

    def bind(self, record):
        self.args.catalog = record['endpoints']['catalog']
        self.args.player = record['endpoints'].get('player')
        self.contexts = cl.connect_context(cl.ContextOptions(
            self.args.catalog, self.args.player, timeout=self.args.timeout,
            max_checkpoint_payload_bytes=self.args.max_checkpoint_payload_bytes), timeout=self.args.timeout)
        self.launcher = Launcher(self.args.identity, self.args.session_id, self.args.host_id, self.args.lock_dir, self.args.catalog,
                                 getattr(self.args, "legacy_lock_dir", None),
                                 instance_id=record["id"] if record["name"] not in ("explicit", "legacy") else None)
        self.store = None
        self.store_error = None
        if self.args.identity is not None:
            self.store = CheckpointStore(self.contexts, self.launcher, self.args.state_chronicle,
                                         self.args.max_checkpoint_payload_bytes, self.args.checkpoint_lookup_max_read_calls,
                                         self.args.timeout)
            try:
                self.store.start()
            except cl.Error as error:
                self.store.state = "unavailable"
                self.store.verdict = f"checkpoint store unavailable at start ({error})"
            except Unavailable as error:
                self.store.state, self.store.verdict = error.state, error.verdict

    def detach(self):
        with self.lock:
            for session in self.sessions.values():
                if not session.closed_verdict:
                    _, persisted = self.close_session(session, "instance detached; open it again")
                    if persisted is not None and persisted['stored'] != 'durable':
                        raise ValueError("session close record failed; instance remains attached")
            if self.store is not None:
                self.store.close()
            if self.launcher is not None:
                self.launcher.release()
            self.instances.detach()
            self.contexts = self.launcher = self.store = None

    # Sessions.

    def session(self, handle, writable=False):
        with self.lock:
            s = self.sessions.get(handle)
        if s is None:
            raise ValueError(f"unknown session_handle {handle!r}")
        if s.closed_verdict:
            raise ValueError(s.closed_verdict)
        if writable and not s.writable:
            raise ValueError("session is read_only; open it with access=read_write to write")
        s.last_used = time.monotonic()
        return s

    def writable_count(self):
        with self.lock:
            return sum(1 for s in self.sessions.values() if s.writable and not s.closed_verdict)

    def need_store(self, takeover=False):
        if self.store is None:
            raise Unavailable("no stable --identity: writable tools need a launcher base slot", "no_identity")
        return self.store.ensure_ready(takeover)

    def close_session(self, s, reason=None):
        """Closes the native session and records the exact incarnation's close record."""
        result = s.native.close(timeout=self.args.timeout)
        persisted = None
        if s.writable:
            try:
                persisted = {"stored": "durable",
                             "checkpoint_id": self.store.persist({(s.label, s.ref.story_id): s.native.checkpoint()})}
            except Unavailable as error:
                persisted = {"stored": "failed", "verdict": error.verdict}
            except cl.Error as error:
                persisted = {"stored": "failed", "verdict": str(error)}
        with self.lock:
            s.closed_verdict = reason or "session closed; open it again"
        return result, persisted

    def _idle_loop(self):
        limit = self.args.idle_close_s
        while not self.stop.wait(min(30.0, max(0.5, limit / 4))):
            now = time.monotonic()
            with self.lock:
                idle = [s for s in self.sessions.values()
                        if s.writable and not s.closed_verdict and now - s.last_used > limit]
                for s in idle:
                    try:
                        self.close_session(s, f"session closed after {limit} s idle; open it again")
                    except cl.Error:
                        pass
                if self.store is not None and not self.writable_count() and now - self.store.last_used > limit:
                    self.store.close()

    def shutdown(self):
        self.stop.set()
        with self.lock:
            live = [s for s in self.sessions.values() if not s.closed_verdict]
        for s in live:
            try:
                self.close_session(s, "server stopped")
            except cl.Error:
                pass
        if self.store is not None:
            self.store.close()
        if self.launcher is not None:
            self.launcher.release()
        self.instances.detach()


def _payload(content, content_type):
    if isinstance(content, str):
        content = {"encoding": "utf8", "data": content}
    if not isinstance(content, dict) or set(content) - {"encoding", "data"} or not isinstance(content.get("data"), str):
        raise ValueError("content must be text or {encoding: utf8|base64, data: string}")
    encoding = content.get("encoding", "utf8")
    if encoding == "utf8":
        return content["data"].encode("utf-8"), content_type or "text/plain; charset=utf-8"
    if encoding == "base64":
        return base64.b64decode(content["data"], validate=True), content_type or "application/octet-stream"
    raise ValueError("content.encoding must be utf8 or base64")


def _prior(p):
    return {"operation_id": p.operation_id, "stored": _stored(p), "outcome": p.outcome.name,
            "status": _status(p.status),
            "receipt": None if p.receipt is None else {
                "event_id": {"story_id": str(p.receipt.event_id.story_id),
                             "writer_id": str(p.receipt.event_id.writer_id),
                             "incarnation": str(p.receipt.event_id.incarnation),
                             "sequence": str(p.receipt.event_id.sequence)},
                "hlc": hlc_json(p.receipt.hlc), "durability": p.receipt.durability.name},
            "landed": None if p.landed is None else _follow_token(p.landed),
            "original_ack_received": p.receipt is not None}


def _stored(p):
    outcome = p.outcome
    if outcome is cl.MemoryOutcome.DURABLE:
        return "durable"
    if outcome is cl.MemoryOutcome.RAM_ONLY_MAY_VANISH:
        return "ram_only_may_vanish"
    if outcome is cl.MemoryOutcome.LANDED:
        return "durable" if p.observed_durability is cl.Durability.DURABLE else "ram_only_may_vanish"
    if outcome in (cl.MemoryOutcome.REJECTED, cl.MemoryOutcome.FENCED):
        return "rejected"
    return "unknown"


def _next_action(state):
    return {cl.SessionState.NEEDS_RECONCILE: "context_reconcile",
            cl.SessionState.FENCED: "context_reconcile with takeover=true after deciding to take the slot over",
            cl.SessionState.TRANSPORT_PENDING: "retry the same operation_id",
            cl.SessionState.CLOSED: "context_open"}.get(state)


def _unavailable(error, **rest):
    return _result(error.verdict, True, store_state=error.state, next_action=error.next_action, **rest)


def create_server(args):
    if not math.isfinite(args.timeout) or not 0 < args.timeout <= 300:
        raise ValueError("timeout must be finite and between 0 and 300 seconds")
    if not 0 < args.max_checkpoint_payload_bytes <= args.keeper_payload_max_bytes:
        raise ValueError("max_checkpoint_payload_bytes must be positive and at most the Keeper payload_max_bytes")
    json_default = getattr(args, "max_json_bytes", DEFAULT_JSON)
    _bound(json_default, 1024, 1 << 20, "max_json_bytes")
    shared = {}

    @asynccontextmanager
    async def lifespan(server):
        state = await asyncio.to_thread(_State, args)
        shared["state"] = state
        try:
            yield state
        finally:
            shared.pop("state", None)
            await asyncio.to_thread(state.shutdown)

    server = MCPServer("chronolog", lifespan=lifespan, instructions=(
        "ChronoLog contexts are durable, time-ordered memories. Keep operation ids stable, read verdict before "
        "interpreting absence, use context_reconcile for uncertain old writes and commit processed follow tokens "
        "through context_checkpoint."))

    def threaded(fn):
        @wraps(fn)
        def locked(*positional, **keywords):
            state = shared["state"]
            if fn.__name__.startswith('instance_'):
                with state.lock:
                    if fn.__name__ == 'instance_control' and state.active_calls:
                        raise ValueError('context tools are running; retry instance control after they finish')
                    return fn(*positional, **keywords)
            with state.lock:
                state.active_calls += 1
            try:
                return fn(*positional, **keywords)
            finally:
                with state.lock:
                    state.active_calls -= 1
        return _threaded(locked)

    def st():
        state = shared["state"]
        if state.contexts is None:
            raise Unavailable('no instance bound; use instance_control action=up name=default create=true',
                              'unbound', 'instance_control up')
        return state

    @server.tool()
    @threaded
    def instance_list(probe: bool = False) -> str:
        """Discover local registered instances and this server's binding. probe=false reads registry metadata;
        probe=true checks endpoints. Returns owner, paths, tiers, policy, clock and attach holders when available.
        External records may omit fields. No credentials are returned."""
        state = shared["state"]
        records = state.instances.listing(probe)
        return compact(_result(f"{len(records)} instances", True, instances=records,
                               bound_instance_id=None if state.instances.record is None else state.instances.record['id']))

    @server.tool()
    @threaded
    def instance_control(action: str, name: str = 'default', create: bool = False,
                         on_last_detach: str | None = None, idle_grace_s: float | None = None,
                         force: bool = False) -> str:
        """up boots and binds a local instance, creating it only with create=true; attach binds a ready instance.
        Rebinding is refused while writable sessions are open. detach closes sessions and drops this server's lease.
        down closes this server's sessions and requests ordered stop; foreign leases require force=true.
        on_last_detach=keep|stop and idle_grace_s configure creation. Binaries come from CHRONOLOG_BIN_DIR or PATH."""
        state = shared["state"]
        if action not in ('up', 'attach', 'detach', 'down'):
            raise ValueError('action must be up, attach, detach or down')
        if on_last_detach not in (None, 'keep', 'stop'):
            raise ValueError('on_last_detach must be keep or stop')
        if idle_grace_s is not None and (not math.isfinite(idle_grace_s) or idle_grace_s < 0):
            raise ValueError('idle_grace_s must be finite and nonnegative')
        if action in ('up', 'attach'):
            current = state.instances.record
            try:
                _, target = state.instances.selected(name)
            except ValueError:
                target = None
            same = current is not None and target is not None and current['id'] == target['id']
            if state.writable_count() and not same:
                return compact(_result('rebind refused: close writable sessions with context_close first', True,
                                       changed=False, instance=state.instances.status()))
            if action == 'up':
                state.instances.up(name, create, on_last_detach, idle_grace_s)
            if not same:
                # Validate the destination before closing the current connection.
                _, target = state.instances.selected(name)
                if target['state'] != 'ready':
                    raise ValueError(f'instance {name} is not ready; use instance_control action=up')
                state.detach()
                record = state.instances.attach(name)
                try:
                    state.bind(record)
                except BaseException:
                    if state.launcher is not None:
                        state.launcher.release()
                    state.instances.detach()
                    state.contexts = state.launcher = state.store = None
                    raise
        else:
            if action == 'down':
                _, target = state.instances.selected(name)
                if state.instances.record is not None and target['id'] == state.instances.record['id']:
                    state.detach()
                state.instances.down(name, force)
            else:
                state.detach()
        return compact(_result(f'instance {action} complete', True, changed=True, instance=state.instances.status()))

    def base():
        return args.identity or "anonymous"

    def resolve(s, name, ref_token, create):
        if ref_token is not None:
            story, chronicle, story_name = untoken("r1", ref_token)
            return cl.ContextRef(int(story), chronicle, story_name)
        if not isinstance(name, str) or not name:
            raise ValueError("pass a context name or a ref_token")
        if create:
            return s.contexts.ensure_context(args.chronicle, name, timeout=args.timeout)
        for ref in s.contexts.list_contexts(args.chronicle, timeout=args.timeout):
            if ref.name == name:
                return ref
        raise cl.NotFound(cl.Status(cl.StatusCode.NOT_FOUND, f"no context {name!r} in chronicle {args.chronicle!r}"))

    def ref_token(ref):
        return token("r1", [str(ref.story_id), ref.chronicle, ref.name])

    def describe(s):
        status = s.native.status()
        identity = cl.AgentIdentity(base(), s.label)
        return {"session_handle": s.handle, "ref_token": ref_token(s.ref),
                "context": {"story_id": str(s.ref.story_id), "chronicle": s.ref.chronicle, "name": s.ref.name},
                "access": s.access.name.lower(),
                "identity": {"agent_id": identity.agent_id, "label": identity.slot,
                             "writer_identity": "agent-context/v2:" + compact([identity.agent_id, identity.slot])},
                "writer": _stamp(status.writer), "state": status.state.name,
                "next_action": _next_action(status.state) if s.writable else None}

    @server.tool()
    @threaded
    def context_open(name: str | None = None, ref_token: str | None = None, agent: str = "self",
                     create: bool = False, access: str = "read_write", resume: bool = False,
                     checkpoint_id: str | None = None) -> str:
        """Open a context (a story in the launcher's chronicle) by name or ref_token for an agent label. A writable
        open always checks the latest acquisition and close record for this label and context: an unclosed record
        returns NeedsReconcile (this launcher's dead owner) or Fenced with takeover required (foreign, still locked
        or unknown owner) without acquiring. resume=true or checkpoint_id restores the processed follow position
        only; it never skips crash detection."""
        s = st()
        if not LABEL.fullmatch(agent or ""):
            raise ValueError("agent label must match [A-Za-z0-9][A-Za-z0-9._-]{0,63}")
        if access not in ("read_write", "read_only"):
            raise ValueError("access must be read_write or read_only")
        mode = cl.Access.READ_WRITE if access == "read_write" else cl.Access.READ_ONLY
        try:
            ref = resolve(s, name, ref_token, create)
        except cl.NotFound as error:
            return compact(_result(str(error.status.message), True, session_handle=None, status=_status(error.status)))
        with s.lock:
            for existing in s.sessions.values():
                if (existing.label, existing.ref.story_id, existing.access) == (agent, ref.story_id, mode) and \
                        not existing.closed_verdict and existing.native.status().state is not cl.SessionState.CLOSED:
                    return compact(_result("already open; the same handle and acquisition", True,
                                           checkpoint_id=s.store.last_id if s.store else None, **describe(existing)))
        identity = cl.AgentIdentity(base(), agent)
        options = cl.OpenOptions(access=mode, session_id=args.session_id)
        named = None
        if mode is cl.Access.READ_WRITE:
            try:
                s.need_store()
            except Unavailable as error:
                return compact(_unavailable(error, session_handle=None))
            with s.store.lock:
                if checkpoint_id is not None:
                    named = s.store.read_checkpoint(checkpoint_id, agent, ref.story_id)
                latest = s.store.resume_for(agent, ref)
                if latest is not None and not resume:
                    latest = replace(latest, processed_after=None)
                if named is not None:
                    # A named checkpoint selects processing state only; newer acquisition state stays.
                    latest = replace(latest or named, processed_after=named.processed_after,
                                     causal_floor=max(named.causal_floor, (latest or named).causal_floor))
                try:
                    s.store.admit_session(agent, ref, s.writable_count())
                except Unavailable as error:
                    return compact(_unavailable(error, session_handle=None))
                options = cl.OpenOptions(access=mode, session_id=args.session_id, resume=latest,
                                         ownership=s.launcher.provenance())
                try:
                    native = s.contexts.open(ref, identity, options=options, timeout=args.timeout)
                except cl.Error as error:
                    return compact(_result(f"open refused: {error.status.code.name}", True, session_handle=None,
                                           status=_status(error.status)))
                status = native.status()
                checkpoint = native.checkpoint()
                acquired = status.state is cl.SessionState.READY and (
                    latest is None or latest.writer != checkpoint.writer or latest.acquisition_closed)
                if acquired:
                    try:
                        s.store.persist({(agent, ref.story_id): checkpoint})
                    except (Unavailable, cl.Error) as error:
                        native.close(timeout=args.timeout)
                        return compact(_result(f"acquisition record not persisted, writer released: {error}", True,
                                               session_handle=None))
        else:
            if resume or checkpoint_id:
                if s.store is None:
                    raise ValueError("resume needs a stable --identity checkpoint store")
                if checkpoint_id is not None:
                    named = s.store.read_checkpoint(checkpoint_id, agent, ref.story_id)
                else:
                    named = s.store.resume_for(agent, ref)
                if named is not None:
                    options = cl.OpenOptions(access=mode, session_id=args.session_id,
                                             resume=cl.Checkpoint(identity, ref, causal_floor=named.causal_floor,
                                                                  processed_after=named.processed_after))
            try:
                native = s.contexts.open(ref, identity, options=options, timeout=args.timeout)
            except cl.Error as error:
                return compact(_result(f"open refused: {error.status.code.name}", True, session_handle=None,
                                       status=_status(error.status)))
        handle = f"h-{agent}-{secrets.token_hex(6)}"
        session = _Session(handle, agent, ref, mode, native)
        with s.lock:
            s.sessions[handle] = session
        info = describe(session)
        state = native.status().state
        verdict = {cl.SessionState.READY: "open",
                   cl.SessionState.NEEDS_RECONCILE: "open in NeedsReconcile: an unclosed acquisition of this "
                                                    "launcher's slot; call context_reconcile before writing",
                   cl.SessionState.FENCED: "open in Fenced: an unclosed record of another or unknown owner; "
                                           "takeover required"}.get(state, f"open in {state.name}")
        if mode is cl.Access.READ_WRITE and s.store.prior_unknown_below is not None and state is not \
                cl.SessionState.READY:
            verdict += f"; the checkpoint store's prior state is unknown below {hlc_json(s.store.prior_unknown_below)}"
        return compact(_result(verdict, True, takeover_required=state is cl.SessionState.FENCED,
                               processed_after=None if (p := native.checkpoint().processed_after) is None
                               else _follow_token(p),
                               checkpoint_id=s.store.last_id if s.store else None, **info))

    @server.tool()
    @threaded
    def context_remember(session_handle: str, operation_id: str, content: str | dict,
                         content_type: str | None = None, attributes: dict[str, str] | None = None,
                         trace_id: str | None = None, span_id: str | None = None, durability: str = "durable",
                         physical_ns: str | None = None, resend_after_absent: bool = False) -> str:
        """Store one memory. Reuse operation_id when retrying after an error or timeout: the same id with the same
        content returns its known outcome and never stores twice; a new id is a new memory. stored is durable,
        ram_only_may_vanish, rejected or unknown; unknown is resolved with context_reconcile."""
        s = st()
        session = s.session(session_handle, writable=True)
        if durability not in ("durable", "accepted"):
            raise ValueError("durability must be durable or accepted")
        payload, kind = _payload(content, content_type)
        envelope = cl.Envelope(payload, kind, attributes, bytes.fromhex(trace_id) if trace_id else None,
                               bytes.fromhex(span_id) if span_id else None)
        try:
            s.need_store()
            if session.persist_required:
                s.store.persist({(session.label, session.ref.story_id): session.native.checkpoint()})
                session.persist_required = False
            s.store.admit(session.label, session.ref.story_id, session.native.checkpoint(), operation_id,
                          s.writable_count())
        except Unavailable as error:
            return compact(_unavailable(error, stored="rejected", outcome="REJECTED", receipt=None))
        memory = cl.Memory(operation_id, envelope,
                           cl.Durability.DURABLE if durability == "durable" else cl.Durability.ACCEPTED,
                           cl.TimeReading(int(physical_ns)) if physical_ns is not None else None)
        try:
            result = session.native.remember(memory, options=cl.RememberOptions(resend_after_absent),
                                             timeout=args.timeout)
        except cl.Error as error:
            # Refused before dispatch: a changed digest for a retained id, malformed content or a reserved key.
            state = session.native.status().state
            return compact(_result(f"rejected before dispatch: {error.status.code.name}: {error.status.message}",
                                   True, operation_id=operation_id, stored="rejected", outcome="REJECTED",
                                   status=_status(error.status), receipt=None, state=state.name,
                                   next_action=_next_action(state)))
        status = session.native.status()
        current = _prior(result.current)
        persisted = None
        if current["stored"] not in ("durable", "ram_only_may_vanish") or result.state is not cl.SessionState.READY:
            # Record the unresolved ids so a restart's reconcile can name them.
            try:
                s.store.persist({(session.label, session.ref.story_id): session.native.checkpoint()})
                persisted = "durable"
            except (Unavailable, cl.Error) as error:
                persisted = f"failed: {error}"
        verdict = f"{current['stored']}: {result.current.outcome.name}"
        if result.blocking_operation_id:
            verdict += f"; blocked behind {result.blocking_operation_id}"
        return compact(_result(
            verdict, current["stored"] != "unknown", **current,
            resolved_prior=[_prior(p) for p in result.resolved_prior],
            blocking_operation_id=result.blocking_operation_id, state=result.state.name,
            unresolved_operation_ids=list(status.unresolved_operations),
            permanently_unknown_operation_ids=list(status.permanently_unknown_operations),
            next_action=_next_action(result.state), checkpoint_persisted=persisted))

    def at_value(session, text, name):
        if text is None:
            return None
        story, physical, logical = untoken("a1", text)
        if int(story) != session.ref.story_id:
            raise ValueError(f"{name} token belongs to another context")
        return cl.Hlc(int(physical), int(logical))

    def fit(events, view, budget, keep_first, allow_empty=False):
        """The longest prefix (or suffix when keep_first is False) of events whose JSON fits the budget. Unless
        allow_empty, the first event is admitted even when it alone exceeds the budget."""
        projected = [_event(e, view) for e in events]
        total, count = 0, 0
        for item in projected if keep_first else reversed(projected):
            size = _size(item) + 1
            if (count or allow_empty) and total + size > budget:
                break
            total, count = total + size, count + 1
        return projected, count, total

    @server.tool()
    @threaded
    def context_recall(session_handle: str, cursor: str | None = None, start: str | None = None,
                       end: str | None = None, max_events: int = DEFAULT_EVENTS, max_json_bytes: int = json_default,
                       view: str = "compact", max_read_calls: int = 32,
                       since: int | str | None = None, until: int | str | None = None) -> str:
        """Read a context in Replay order. Omitted start is the beginning, omitted end a verified cut; start and end
        are opaque `at` tokens from returned events (end is exclusive). Pass next_cursor alone to continue; it
        carries the range. since/until accept int64 nanoseconds or RFC 3339, exclusive of start/end/cursor;
        until is exclusive. They map to Hlc{t, 0}: Keeper CLOCK_REALTIME at acceptance, with HLC lead at most D
        (61 s by default), so these are acceptance-time ranges (I8.7, I8.8), not writer-time readPhysical ranges.
        HLC coverage can be complete (I6.1). Check verdict and answer_complete before treating a missing event as absent."""
        s = st()
        session = s.session(session_handle)
        _bound(max_events, 1, 1000, "max_events")
        _bound(max_json_bytes, 1024, 1 << 20, "max_json_bytes")
        _bound(max_read_calls, 1, 256, "max_read_calls")
        if view not in ("compact", "full"):
            raise ValueError("view must be compact or full")
        if cursor is not None and (start is not None or end is not None):
            raise ValueError("a cursor carries its range; pass it alone")
        if (since is not None or until is not None) and any(v is not None for v in (start, end, cursor)):
            raise ValueError("since/until are exclusive of start/end/cursor")
        lower = acceptance_bound(since, "since") if since is not None else at_value(session, start, "start")
        upper = acceptance_bound(until, "until") if until is not None else at_value(session, end, "end")
        if lower is not None and upper is not None and lower >= upper:
            raise ValueError("range start must be before its exclusive end")
        options = cl.RecallOptions(start=lower, end=upper,
                                   cursor=cursor, limits=cl.PageLimits(max_events, max_json_bytes),
                                   max_read_calls=max_read_calls)
        page = session.native.recall(options=options, timeout=args.timeout)
        for _ in range(3):
            projected, count, _ = fit(page.events, view, max_json_bytes - 2048, True)
            if count == len(page.events):
                break
            # Fix the range the first answer verified, then let the native layer cut at an HLC group boundary.
            pinned = options if cursor is not None else cl.RecallOptions(
                start=page.range.start if page.range else None, end=page.range.end if page.range else None,
                max_read_calls=max_read_calls)
            options = cl.RecallOptions(start=pinned.start, end=pinned.end, cursor=cursor,
                                       limits=cl.PageLimits(count, max_json_bytes), max_read_calls=max_read_calls)
            page = session.native.recall(options=options, timeout=args.timeout)
        projected = [_event(e, view) for e in page.events]
        verdict = (f"{len(projected)} events; " + ("answer complete" if page.answer_complete else
                   "more certified pages follow" if page.next_cursor else
                   "INCOMPLETE: coverage unresolved, retry this range"))
        out = _result(verdict, page.answer_complete, page.has_more, page.next_cursor, events=projected,
                      range=_range(page.range), completion_range=_range(page.completion_range),
                      completion=_completion(page.completion), status=_status(page.stream_status),
                      limited=page.limited.name, cut_covers_causal_floor=page.cut_covers_causal_floor,
                      delivered_prefix_end=hlc_json(page.delivered_prefix_end),
                      follow_token=None if page.after is None else _follow_token(page.after), view=view,
                      omitted=OMITTED if view == "compact" else [])
        size = _size(out)
        if size > max_json_bytes:
            out["json_overrun"] = "first event or equal-HLC group exceeds max_json_bytes"
        out["json_bytes"] = size
        return compact(out)

    @server.tool()
    @threaded
    def context_latest(session_handle: str, n: int = 10, before: str | None = None, max_read_calls: int = 32,
                       max_json_bytes: int = json_default, view: str = "compact",
                       until: int | str | None = None) -> str:
        """The last n events of a context before an optional `at` token (exclusive), oldest first, at a disclosed
        verified as_of. until accepts int64 nanoseconds or RFC 3339, exclusive of before, as Hlc{t, 0}.
        This is Keeper acceptance time, CLOCK_REALTIME with HLC lead at most D (61 s by default; I8.7, I8.8),
        not writer-time readPhysical. HLC coverage can be complete (I6.1). selection_complete says the last-n selection is proven."""
        s = st()
        session = s.session(session_handle)
        _bound(n, 1, 1000, "n")
        _bound(max_json_bytes, 1024, 1 << 20, "max_json_bytes")
        _bound(max_read_calls, 1, 256, "max_read_calls")
        if view not in ("compact", "full"):
            raise ValueError("view must be compact or full")
        if until is not None and before is not None:
            raise ValueError("until is exclusive of before")
        upper = acceptance_bound(until, "until") if until is not None else at_value(session, before, "before")
        result = session.native.latest(n, options=cl.LatestOptions(
            before=upper, limits=cl.PageLimits(max(n, 1), max_json_bytes),
            max_read_calls=max_read_calls), timeout=args.timeout)
        page = result.page
        projected, count, _ = fit(page.events, view, max_json_bytes - 2048, False)
        limited = page.limited.name
        selection_complete = result.selection_complete
        if count < len(projected):
            projected, limited, selection_complete = projected[len(projected) - count:], "BYTES", False
        delivered = selection_complete and count == len(page.events)
        verdict = (f"{len(projected)} latest events" + ("; selection proven at as_of" if delivered else
                   "; NOT proven: provisional or delivery-limited, ask for fewer or retry"))
        return compact(_result(verdict, delivered, not delivered, None, events=projected,
                               as_of=hlc_json(result.as_of),
                               as_of_token=None if result.as_of is None else _at(session.ref.story_id, result.as_of),
                               selection_complete=selection_complete, range=_range(page.range),
                               completion_range=_range(page.completion_range),
                               completion=_completion(page.completion), status=_status(page.stream_status),
                               limited=limited, view=view, omitted=OMITTED if view == "compact" else []))

    @server.tool()
    @threaded
    def context_follow(subscriptions: list[dict], timeout_s: float = 5.0, max_events: int = DEFAULT_EVENTS,
                       max_json_bytes: int = json_default, view: str = "compact") -> str:
        """Wait for new events on one or more contexts under one shared deadline. Each subscription is
        {session_handle, from: "now" | "beginning" | follow_token}. Returns when any context has events or the wait
        ends idle; every context gets a next follow_token, which survives server restarts. Never complete."""
        s = st()
        if not math.isfinite(timeout_s) or not 0 < timeout_s <= 60:
            raise ValueError("timeout_s must be between 0 and 60 seconds")
        _bound(max_events, 1, 1000, "max_events")
        _bound(max_json_bytes, 1024, 1 << 20, "max_json_bytes")
        if view not in ("compact", "full"):
            raise ValueError("view must be compact or full")
        if not subscriptions:
            raise ValueError("subscriptions must name at least one session_handle")
        inputs, sessions, origins = [], [], []
        for item in subscriptions:
            session = s.session(item.get("session_handle"))
            origin = item.get("from", "now")
            if origin == "now":
                inputs.append(cl.FollowInput(session.native, cl.FollowFrom.NOW))
            elif origin == "beginning":
                inputs.append(cl.FollowInput(session.native, cl.FollowFrom.BEGINNING))
            else:
                after = position_of(untoken("f1", origin))
                if after.id.story_id != session.ref.story_id:
                    raise ValueError("follow_token belongs to another context")
                inputs.append(cl.FollowInput(session.native, cl.FollowFrom.POSITION, after))
            sessions.append(session)
            origins.append(origin)
        result = s.contexts.follow(inputs, options=cl.FollowOptions(cl.PageLimits(max_events, max_json_bytes),
                                                                    timeout_s), timeout=timeout_s + args.timeout)
        budget = max_json_bytes - 1024 - 512 * len(sessions)
        pages, delivered = [], 0
        for session, origin, page in zip(sessions, origins, result.pages):
            events = page.page.events
            # Only the first delivering context may exceed the budget with its first event.
            projected, count, used = fit(events, view, budget, True, allow_empty=delivered > 0)
            budget -= used
            delivered += count
            if count == len(events):
                resume = page.resume
            elif count:
                resume = cl.Position(events[count - 1].hlc, events[count - 1].id)
            elif origin == "beginning":
                resume = cl.Position(cl.Hlc(), cl.EventId(session.ref.story_id))
            elif origin == "now":
                resume = None if page.starting_cut is None else cl.Position(page.starting_cut,
                                                                           cl.EventId(session.ref.story_id))
            else:
                resume = position_of(untoken("f1", origin))
            pages.append({"session_handle": session.handle, "events": projected[:count],
                          "next_follow_token": None if resume is None else _follow_token(resume),
                          "starting_cut": hlc_json(page.starting_cut), "status": _status(page.page.stream_status),
                          "completion": _completion(page.page.completion),
                          "limited": "BYTES" if count < len(events) else page.page.limited.name,
                          "uncertified_route_keepers": list(page.uncertified_route_keepers)})
        verdict = ("idle: no new events; resume with each next_follow_token" if result.idle and not delivered else
                   f"{delivered} new events" if result.status.ok else
                   f"follow stopped: {result.status.code.name}: {result.status.message}")
        return compact(_result(verdict, False, False, None, pages=pages, idle=result.idle and not delivered,
                               status=_status(result.status), view=view,
                               omitted=OMITTED if view == "compact" else []))

    @server.tool()
    @threaded
    def context_reconcile(session_handle: str | None = None, operation_ids: list[str] | None = None,
                          takeover: bool = False, max_read_calls: int = 32) -> str:
        """Recover uncertain writes after an error, a timeout, a fence or a restart: acquires a successor writer,
        writes a durable resume marker and reports each operation LANDED, ABSENT or UNKNOWN. Pass every operation_id
        for which you have not seen an outcome; omitted ids may duplicate. takeover=true is the deliberate decision
        to fence another or unknown owner. With session_handle omitted it recovers the checkpoint store only."""
        s = st()
        _bound(max_read_calls, 1, 256, "max_read_calls")
        try:
            store_result = s.need_store(takeover)
        except Unavailable as error:
            return compact(_unavailable(error, attempted=False, supply_all_unseen_operation_ids=True,
                                        omitted_operation_ids_may_duplicate=True))
        if session_handle is None:
            return compact(_result(s.store.verdict, True, attempted=store_result is not None,
                                   store=s.store.status(s.writable_count()),
                                   supply_all_unseen_operation_ids=True, omitted_operation_ids_may_duplicate=True))
        session = s.session(session_handle, writable=True)
        try:
            result = session.native.reconcile(options=cl.ReconcileOptions(tuple(operation_ids or ()), takeover,
                                                                          max_read_calls), timeout=args.timeout)
        except cl.Error as error:
            state = session.native.status().state
            return compact(_result(f"not attempted: {error.status.code.name}: {error.status.message}", True,
                                   attempted=False, status=_status(error.status), state=state.name,
                                   next_action=_next_action(state), supply_all_unseen_operation_ids=True,
                                   omitted_operation_ids_may_duplicate=True))
        persisted = None
        if result.attempted:
            try:
                s.store.persist({(session.label, session.ref.story_id): session.native.checkpoint()})
                persisted = "durable"
            except (Unavailable, cl.Error) as error:
                session.persist_required = True
                persisted = f"failed: {error}; writes wait until the transition is persisted"
        state = session.native.status().state
        verdict = ("reconciled" + ("; proof complete" if result.proof_complete else
                                   "; proof incomplete, unresolved ids stay permanently UNKNOWN")
                   if result.attempted else f"not attempted: {result.status.code.name}: {result.status.message}")
        return compact(_result(
            verdict, result.attempted and result.proof_complete, False, None, attempted=result.attempted,
            proof_complete=result.proof_complete, writer=_stamp(result.writer), marker_hlc=hlc_json(result.marker_hlc),
            proof_range=_range(result.range),
            operations=[{"operation_id": op.operation_id, "outcome": op.outcome.name,
                         "landed": None if op.landed is None else _follow_token(op.landed),
                         "observed_durability": op.observed_durability.name} for op in result.operations],
            permanently_unknown_operation_ids=list(result.permanently_unknown_operations),
            completion=_completion(result.completion), status=_status(result.status), state=state.name,
            next_action=_next_action(state), checkpoint_persisted=persisted,
            supply_all_unseen_operation_ids=result.supply_all_unseen_operation_ids,
            omitted_operation_ids_may_duplicate=result.omitted_operation_ids_may_duplicate))

    @server.tool()
    @threaded
    def context_checkpoint(session_handle: str | None = None, processed: list[str] | None = None) -> str:
        """Durably persist session state for one handle or every writable handle, after acknowledging processed
        follow tokens whose effects you have committed. stored=durable only after the checkpoint is Durable; the
        returned checkpoint_id can be passed to context_open."""
        s = st()
        with s.lock:
            targets = [s.session(session_handle)] if session_handle else [
                x for x in s.sessions.values() if not x.closed_verdict]
        acknowledged = []
        for text in processed or []:
            at = position_of(untoken("f1", text))
            matched = [x for x in targets if x.ref.story_id == at.id.story_id]
            if not matched:
                raise ValueError("a processed token belongs to no selected session")
            for x in matched:
                x.native.acknowledge_processed(at)
            acknowledged.append(text)
        writable = [x for x in targets if x.writable]
        if not writable:
            return compact(_result("nothing persisted: read_only sessions are not checkpointed", True,
                                   stored="none", acknowledged=acknowledged, checkpoint_id=None))
        try:
            s.need_store()
            checkpoint_id = s.store.persist({(x.label, x.ref.story_id): x.native.checkpoint() for x in writable})
        except (Unavailable, cl.Error) as error:
            return compact(_result(f"checkpoint not durable: {error}", True, stored="failed",
                                   acknowledged=acknowledged, checkpoint_id=None))
        return compact(_result("checkpoint durable", True, stored="durable", checkpoint_id=checkpoint_id,
                               acknowledged=acknowledged, sessions=[x.handle for x in writable]))

    @server.tool()
    @threaded
    def context_close(session_handle: str) -> str:
        """Close a session: release its writer and durably record the close of that exact incarnation. Closing
        neither destroys the context nor stops the stack."""
        s = st()
        session = s.session(session_handle)
        try:
            result, persisted = s.close_session(session)
        except cl.Error as error:
            return compact(_result(f"close failed: {error.status.code.name}", True, status=_status(error.status)))
        clean = persisted is None or persisted["stored"] == "durable"
        verdict = "closed" if clean else "released but the close record failed: conservatively unclosed"
        return compact(_result(verdict, clean, release_committed=result.release_committed, fenced=result.fenced,
                               close_record=persisted))

    @server.tool()
    @threaded
    def context_list(chronicle: str | None = None) -> str:
        """List contexts in a chronicle (default: the launcher's). Metadata only: no acquisitions and no claim that
        any event history is complete."""
        s = st()
        refs = s.contexts.list_contexts(chronicle or args.chronicle, timeout=args.timeout)
        return compact(_result(f"{len(refs)} contexts", True, chronicle=chronicle or args.chronicle,
                               contexts=[{"name": r.name, "story_id": str(r.story_id), "ref_token": ref_token(r)}
                                         for r in refs]))

    @server.tool()
    @threaded
    def context_status(session_handle: str | None = None, agent: str | None = None) -> str:
        """Session states, writer stamps, causal floors, unresolved and permanently UNKNOWN operation ids, the
        checkpoint store and the retry window. Operation ids older than the retained window can be new again."""
        s = st()
        with s.lock:
            chosen = [s.session(session_handle)] if session_handle else [
                x for x in s.sessions.values() if not x.closed_verdict and (agent is None or x.label == agent)]
        sessions = []
        for x in chosen:
            status = x.native.status()
            sessions.append({**describe(x), "causal_floor": hlc_json(status.causal_floor),
                             "blocking_operation_id": status.blocking_operation_id,
                             "unresolved_operation_ids": list(status.unresolved_operations),
                             "permanently_unknown_operation_ids": list(status.permanently_unknown_operations),
                             "reconcile_attempted": status.reconcile_attempted,
                             "persist_required": x.persist_required})
        store = s.store.status(s.writable_count()) if s.store else {"state": "no_identity"}
        return compact(_result(f"{len(sessions)} sessions; checkpoint store {store['state']}", True,
                               launcher={"identity": args.identity, "session_id": args.session_id,
                                         "host_id": args.host_id, "slot_locked_here": s.launcher.held,
                                         "chronicle": args.chronicle},
                               sessions=sessions, checkpoint_store=store, instance=s.instances.status(),
                               retry_window={"max_completed_operations": 10000,
                                             "note": "an operation_id outside the retained window can be new again"},
                               capabilities={"recall": "certified pages with local HLC-group cuts",
                                             "wire": ["ReadRequest.max_events", "AppendResult.rejection"]}))

    return server


def main():
    parser = argparse.ArgumentParser(description="ChronoLog Context tools over MCP")
    parser.add_argument("--catalog", default=os.getenv("CHRONOLOG_CATALOG"))
    parser.add_argument("--player", default=os.getenv("CHRONOLOG_PLAYER"))
    parser.add_argument("--chronicle", default=os.getenv("CHRONOLOG_CHRONICLE", "chronolog"),
                        help="the launcher's chronicle that context names resolve in")
    parser.add_argument("--identity", default=os.getenv("CHRONOLOG_MCP_IDENTITY",
                                                        os.getenv("CHRONOLOG_WRITER_IDENTITY")),
                        help="stable launcher base slot; writable tools need it")
    parser.add_argument("--session-id", default=os.getenv("CHRONOLOG_MCP_SESSION_ID") or uuid.uuid4().hex,
                        help="the launcher's run session id; fresh per independent run")
    parser.add_argument("--host-id", default=os.getenv("CHRONOLOG_MCP_HOST_ID") or socket.gethostname())
    parser.add_argument("--lock-dir", default=os.getenv("CHRONOLOG_MCP_LOCK_DIR") or os.path.join(
        chronolog_home(), "locks"))
    parser.add_argument("--max-json-bytes", type=int, default=DEFAULT_JSON,
                        help="default tool JSON budget; use 14336 for clio-coder (one event or HLC group may exceed it)")
    parser.add_argument("--state-chronicle", default=os.getenv("CHRONOLOG_MCP_STATE_CHRONICLE", "agent-state"))
    parser.add_argument("--max-checkpoint-payload-bytes", type=int, default=1 << 20)
    parser.add_argument("--keeper-payload-max-bytes", type=int, default=1 << 20,
                        help="the deployment's Keeper payload_max_bytes")
    parser.add_argument("--checkpoint-lookup-max-read-calls", type=int, default=32)
    parser.add_argument("--idle-close-s", type=float, default=1800, help="close idle writable sessions; 0 disables")
    parser.add_argument("--timeout", type=float, default=10)
    parser.add_argument("--transport", choices=["stdio", "http"], default=os.getenv("MCP_TRANSPORT", "stdio"))
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8000)
    args = parser.parse_args()
    args.legacy_lock_dir = args.lock_dir
    if args.lock_dir == os.path.join(chronolog_home(), "locks") and not os.getenv("CHRONOLOG_MCP_LOCK_DIR"):
        args.legacy_lock_dir = os.path.join(
            os.getenv("XDG_RUNTIME_DIR") or os.path.expanduser("~/.cache"), "chronolog-mcp")
    server = create_server(args)

    def stop(*_):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, stop)
    try:
        if args.transport == "http":
            server.run(transport="streamable-http", host=args.host, port=args.port)
        else:
            server.run(transport="stdio")
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
