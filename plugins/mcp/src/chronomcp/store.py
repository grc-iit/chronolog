"""The launcher's stable slot, its host-local locks and the shared _checkpoints story (RFC-F sections 1 and 7).

One control writer per stable launcher base slot persists DURABLE aggregates keyed by (label, story_id). Every
aggregate is self-contained; the newest one found by the bounded n=1 lookup is the state. A lookup before the control
Acquire is advisory; the authoritative lookup runs at or above successor(m_ctrl), an own acknowledged write of the new
control incarnation (RFC-FG C2)."""
import base64
from dataclasses import dataclass, replace
import fcntl
import hashlib
import json
import ipaddress
import os
import pathlib
import threading
import time

import chronolog as cl

AGGREGATE_TYPE = "application/vnd.chronolog.mcp-checkpoints+json"
AGGREGATE_VERSION = "chronolog-mcp-checkpoints/v1"
CONTROL_SLOT = "checkpoints"
MAX_HLC = cl.Hlc(2**63 - 1, 2**32 - 1)
WORST_ID = "\x01" * 128


def compact(value):
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False)


def hlc_json(h):
    return None if h is None else [str(h.physical_ns), h.logical]


def hlc_value(v):
    return None if v is None else cl.Hlc(int(v[0]), int(v[1]))


def successor(h):
    return cl.Hlc(h.physical_ns, h.logical + 1) if h.logical < 2**32 - 1 else cl.Hlc(h.physical_ns + 1, 0)


def token(kind, values):
    return kind + "." + base64.urlsafe_b64encode(compact(values).encode()).decode().rstrip("=")


def untoken(kind, text):
    if not isinstance(text, str) or not text.startswith(kind + "."):
        raise ValueError(f"not a {kind} token")
    body = text[len(kind) + 1:]
    try:
        return json.loads(base64.urlsafe_b64decode(body + "=" * (-len(body) % 4)))
    except ValueError as error:
        raise ValueError(f"malformed {kind} token") from error


def position_values(p):
    return [str(p.id.story_id), str(p.hlc.physical_ns), p.hlc.logical, str(p.id.writer_id), str(p.id.incarnation),
            str(p.id.sequence)]


def position_of(values):
    story, physical, logical, writer, incarnation, sequence = values
    return cl.Position(cl.Hlc(int(physical), int(logical)),
                       cl.EventId(int(story), int(writer), int(incarnation), int(sequence)))


class Unavailable(Exception):
    """The checkpoint store cannot serve a writable operation; the message is the verdict."""

    def __init__(self, verdict, state, next_action=None):
        super().__init__(verdict)
        self.verdict, self.state, self.next_action = verdict, state, next_action


def catalog_key(catalog):
    targets = []
    for target in catalog.split(","):
        target = target.strip()
        if target.startswith("unix:"):
            targets.append("unix:" + os.path.abspath(target[5:]))
            continue
        host, separator, port = target.rpartition(":")
        if not separator:
            raise ValueError("catalog endpoint must include a port")
        host = host.strip("[]").lower().rstrip(".")
        if host == "localhost":
            host = "127.0.0.1"
        else:
            try:
                host = str(ipaddress.ip_address(host))
            except ValueError:
                pass
        targets.append(f"[{host}]:{int(port)}")
    return ",".join(sorted(set(targets)))


class Launcher:
    """The stable base slot, the fresh run session id and the host-local locks held for the launch."""

    def __init__(self, base, session_id, host_id, lock_dir, catalog, legacy_lock_dir=None, instance_id=None):
        self.base, self.session_id, self.host_id = base, session_id, host_id
        self.lock_dir = pathlib.Path(lock_dir)
        self.held = False
        self._files = []
        if base is None:
            return
        self.lock_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
        legacy_dir = pathlib.Path(legacy_lock_dir or lock_dir)
        legacy_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
        scope = f"instance:{instance_id}" if instance_id else f"catalog:{catalog_key(catalog)}"
        key = hashlib.sha256(f"{scope}\n{base}".encode()).hexdigest()[:32]
        legacy_key = hashlib.sha256(f"{catalog}\n{base}".encode()).hexdigest()[:32]
        self.slot_path = self.lock_dir / f"slot-{key}.lock"
        legacy_path = legacy_dir / f"slot-{legacy_key}.lock"
        self.hints_path = legacy_dir / f"slot-{legacy_key}.json"
        # Retain the prior provenance while holding both lock keys for the migration release.
        self.lock_id = f"flock:{legacy_path}"
        catalog_slot = self.lock_dir / ("slot-" + hashlib.sha256(
            f"catalog:{catalog_key(catalog)}\n{base}".encode()).hexdigest()[:32] + ".lock")
        paths = [self.slot_path, catalog_slot, legacy_path, self.lock_dir / f"slot-{legacy_key}.lock",
                 self.lock_dir / f"session-{hashlib.sha256(session_id.encode()).hexdigest()[:32]}.lock"]
        for path in dict.fromkeys(paths):
            handle = self._lock(path)
            if handle is None:
                self.release()
                return
            self._files.append(handle)
        self.held = True

    @staticmethod
    def _lock(path):
        handle = os.fdopen(os.open(path, os.O_RDWR | os.O_CREAT | os.O_APPEND, 0o600), "a+")
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            handle.close()
            return None
        handle.seek(0)
        handle.truncate()
        handle.write(str(os.getpid()))
        handle.flush()
        return handle

    def provenance(self):
        return cl.AcquisitionProvenance(self.host_id, self.lock_id)

    def hints(self):
        """Stamps this host last acquired per key; advisory CAS guesses used only after an incomplete lookup."""
        try:
            return json.loads(self.hints_path.read_text())
        except (OSError, ValueError):
            return {}

    def save_hints(self, hints):
        temporary = self.hints_path.with_suffix(".tmp")
        temporary.write_text(compact(hints))
        os.replace(temporary, self.hints_path)

    def release(self):
        for handle in self._files:
            handle.close()
        self._files, self.held = [], False


@dataclass
class Lookup:
    body: dict | None
    event: cl.Event | None
    control: tuple | None  # (control record, event) of the newest aggregate seen, authoritative or not
    complete: bool
    boundary: cl.Hlc


def entry_key(label, story_id):
    return f"{label}\n{story_id}"


class CheckpointStore:
    def __init__(self, contexts, launcher, chronicle, max_payload, lookup_read_calls, timeout):
        self.contexts, self.launcher, self.timeout = contexts, launcher, timeout
        self.chronicle, self.max_payload, self.read_calls = chronicle, max_payload, lookup_read_calls
        self.lock = threading.RLock()
        self.identity = cl.AgentIdentity(launcher.base, CONTROL_SLOT, control=True)
        self.entries = {}
        self.prior_unknown_below = None
        self.session = self.reader = self.ref = None
        self.state, self.verdict = "starting", "checkpoint store not started"
        self.acquisition_receipt = None
        self.pending = None
        self.seq = 0
        self.last_id = None
        self.last_used = time.monotonic()

    # Lookup and recovery.

    def _lookup(self, before=None):
        control, bound = None, before
        for _ in range(8):
            result = self.reader.latest_aggregate(
                AGGREGATE_TYPE, options=cl.LatestOptions(before=bound, max_read_calls=self.read_calls),
                timeout=self.timeout)
            boundary = before or result.as_of or cl.Hlc(time.time_ns())
            if not result.selection_complete:
                return Lookup(None, None, control, False, boundary)
            if not result.page.events:
                return Lookup(None, None, control, True, boundary)
            event = result.page.events[-1]
            try:
                body = json.loads(event.envelope.payload)
                if body.get("v") != AGGREGATE_VERSION or body.get("base") != self.launcher.base:
                    raise ValueError("foreign aggregate")
            except ValueError:
                return Lookup(None, None, control, False, boundary)
            control = control or (body["control"], event)
            if body.get("authoritative"):
                return Lookup(body, event, control, True, boundary)
            bound = event.hlc
        return Lookup(None, None, control, False, before or cl.Hlc(time.time_ns()))

    def _adopt(self, lookup):
        if lookup.body is not None:
            for item in lookup.body["entries"]:
                key = entry_key(item["label"], item["checkpoint"]["context"][0])
                if key not in self.entries:
                    self.entries[key] = cl.decode_checkpoint(compact(item["checkpoint"]).encode())
            self.prior_unknown_below = max(filter(None, [self.prior_unknown_below,
                                                         hlc_value(lookup.body.get("prior_state_unknown_below"))]),
                                           default=None)
        if not lookup.complete:
            self.prior_unknown_below = max(filter(None, [self.prior_unknown_below, lookup.boundary]))

    def start(self):
        with self.lock:
            self.ref = self.contexts.ensure_context(self.chronicle, f"_checkpoints:{self.launcher.base}",
                                                    timeout=self.timeout)
            self.reader = self.contexts.open(self.ref, self.identity, options=cl.OpenOptions(access=cl.Access.READ_ONLY),
                                             timeout=self.timeout)
            if not self.launcher.held:
                self.state = "slot_locked"
                self.verdict = (f"stable slot {self.launcher.base!r} is locked by another launcher on this host "
                                f"({self.launcher.slot_path}); writable tools are refused")
                return
            advisory = self._lookup()
            resume = None
            if advisory.control is not None:
                record, event = advisory.control
                writer = cl.WriterStamp(int(record["writer"][0]), int(record["writer"][1]))
                resume = cl.Checkpoint(self.identity, self.ref, causal_floor=event.hlc, writer=writer,
                                       acquisition=cl.AcquisitionProvenance(
                                           record["host"], record["lock"],
                                           acquisition_record_receipt_hlc=hlc_value(record["acquisition_receipt"])
                                           or event.hlc),
                                       last_own_receipt_hlc=event.hlc)
            elif not advisory.complete:
                hint = self.launcher.hints().get("control")
                resume = cl.Checkpoint(self.identity, self.ref, prior_state_unknown_below=advisory.boundary)
                if hint:
                    resume = replace(resume, writer=cl.WriterStamp(int(hint[0]), int(hint[1])),
                                     acquisition=self.launcher.provenance())
            self.session = self.contexts.open(
                self.ref, self.identity, options=cl.OpenOptions(session_id=self.launcher.session_id, resume=resume,
                                                                ownership=self.launcher.provenance()),
                timeout=self.timeout)
            self._settle(advisory)

    def _settle(self, advisory=None, takeover=False):
        state = self.session.status().state
        if state is cl.SessionState.READY and advisory is not None:
            # A first Acquire with no prior record: the bootstrap aggregate is the own write the authoritative
            # lookup below its HLC needs.
            self._adopt(advisory)
            receipt = self._append(authoritative=False)
            self._adopt(self._lookup(before=receipt.hlc))
            self._append(authoritative=True)
            self._ready()
        elif state in (cl.SessionState.NEEDS_RECONCILE, cl.SessionState.FENCED) and (
                state is cl.SessionState.NEEDS_RECONCILE or takeover):
            result = self.session.reconcile(
                options=cl.ReconcileOptions(takeover=takeover, max_read_calls=self.read_calls), timeout=self.timeout)
            if not (result.attempted and self.session.status().state is cl.SessionState.READY):
                self._refuse(result.status, self.session.status().state)
                return result
            self.acquisition_receipt = None
            if result.marker_hlc is not None:
                self._adopt(self._lookup(before=successor(result.marker_hlc)))
            else:
                self._adopt(Lookup(None, None, None, False, cl.Hlc(time.time_ns())))
            self._append(authoritative=True)
            self._ready()
            return result
        elif state is cl.SessionState.READY:
            self._ready()
        else:
            self._refuse(None, state)

    def _ready(self):
        self.state = "ready"
        self.verdict = "checkpoint store ready"
        if self.prior_unknown_below is not None:
            self.verdict += f"; prior state unknown below {hlc_json(self.prior_unknown_below)}"

    def _refuse(self, status, state):
        story = f"{self.chronicle}/_checkpoints:{self.launcher.base} (story {self.ref.story_id})"
        if state is cl.SessionState.FENCED:
            self.state = "takeover_required"
            self.verdict = (f"checkpoint store {story} has an unclosed control record of another or unknown owner; "
                            "deliberate takeover required")
        else:
            self.state = "needs_reconcile"
            self.verdict = f"checkpoint store {story} needs reconcile"
        if status is not None and not status.ok:
            self.verdict += f" ({status.code.name}: {status.message})"

    def ensure_ready(self, takeover=False):
        """Recovers the control session when a writable tool needs it; raises Unavailable otherwise."""
        with self.lock:
            self.last_used = time.monotonic()
            if self.state == "ready":
                return None
            if self.state == "closed":
                self.session = self.contexts.open(
                    self.ref, self.identity, options=cl.OpenOptions(session_id=self.launcher.session_id,
                                                                    ownership=self.launcher.provenance()),
                    timeout=self.timeout)
                self.acquisition_receipt = None
                if self.session.status().state is cl.SessionState.READY:
                    self._append(authoritative=True)
                    self._ready()
                    return None
            if self.state in ("needs_reconcile", "closed") or (takeover and self.state in (
                    "takeover_required", "checkpoint_store_fenced")):
                result = self._settle(takeover=takeover)
                if self.state == "ready":
                    return result
            raise self.unavailable()

    def unavailable(self):
        action = {"takeover_required": "context_reconcile with takeover=true (session_handle omitted recovers the "
                                       "checkpoint store)",
                  "checkpoint_store_fenced": "context_reconcile with takeover=true only after deciding to fence the "
                                             "other control writer",
                  "needs_reconcile": "context_reconcile"}.get(self.state)
        return Unavailable(self.verdict, self.state, action)

    # Aggregates.

    def _body(self, entries, authoritative=True):
        status = self.session.status()
        return {"v": AGGREGATE_VERSION, "base": self.launcher.base, "authoritative": authoritative,
                "control": {"writer": [str(status.writer.writer_id), str(status.writer.incarnation)],
                            "host": self.launcher.host_id, "lock": self.launcher.lock_id,
                            "session": self.launcher.session_id,
                            "acquisition_receipt": hlc_json(self.acquisition_receipt)},
                "prior_state_unknown_below": hlc_json(self.prior_unknown_below),
                "entries": [{"label": key.split("\n", 1)[0],
                             "checkpoint": json.loads(cl.encode_checkpoint(value))}
                            for key, value in sorted(entries.items())]}

    def encoded_size(self, entries):
        return len(compact(self._body(entries)).encode())

    def reserve(self, sessions):
        """Bytes kept free per writable session for its acquisition, marker, disposition and close updates."""
        sample = cl.Checkpoint(cl.AgentIdentity("a", "b"), cl.ContextRef(1, "c", "n"))
        worst = replace(sample, permanently_unknown_operations=(cl.UnknownOperation(
            WORST_ID, (cl.WriterStamp(2**64 - 1, 2**64 - 1),), cl.HlcRange(MAX_HLC, MAX_HLC), True, b"\xff" * 32),))
        return sessions * (len(cl.encode_checkpoint(worst)) - len(cl.encode_checkpoint(sample)) + 16)

    def _append(self, entries=None, authoritative=True):
        """Appends one DURABLE aggregate; returns its receipt or raises Unavailable without changing state."""
        entries = self.entries if entries is None else entries
        if self.pending is not None:
            prior = self.session.remember(self.pending, timeout=self.timeout)
            self._check(prior)
            self.pending = None
        payload = compact(self._body(entries, authoritative)).encode()
        if len(payload) > self.max_payload:
            raise Unavailable(f"checkpoint aggregate of {len(payload)} bytes exceeds max_checkpoint_payload_bytes "
                              f"{self.max_payload}", "resource_exhausted")
        self.seq += 1
        memory = cl.Memory(f"chronolog-mcp/{self.launcher.session_id}/{self.seq}",
                           cl.Envelope(payload, content_type=AGGREGATE_TYPE))
        result = self.session.remember(memory, timeout=self.timeout)
        if result.current.outcome is cl.MemoryOutcome.UNKNOWN and result.state is cl.SessionState.TRANSPORT_PENDING:
            self.pending = memory
        self._check(result)
        receipt = result.current.receipt
        if self.acquisition_receipt is None:
            self.acquisition_receipt = receipt.hlc
        self.last_id = token("c1", position_values(cl.Position(receipt.hlc, receipt.event_id)))
        return receipt

    def _check(self, result):
        current = result.current
        if current.outcome is cl.MemoryOutcome.DURABLE:
            return
        state = result.state
        rejection = cl.rejection_of(current.status)
        if state is cl.SessionState.FENCED and rejection in (cl.AppendRejection.FENCED_RELEASED,
                                                             cl.AppendRejection.FENCED_SUPERSEDED):
            self.state = "checkpoint_store_fenced"
            self.verdict = (f"checkpoint_store_fenced: control writer agent-context-control/v2:"
                            f"{compact([self.launcher.base, CONTROL_SLOT])} on story {self.ref.story_id} was fenced "
                            f"({rejection.name}); writable tools fail closed, reads continue")
        elif state in (cl.SessionState.NEEDS_RECONCILE, cl.SessionState.FENCED):
            self._refuse(current.status, state)
        raise Unavailable(self.verdict if self.state != "ready" else
                          f"checkpoint aggregate not durable: {current.outcome.name} "
                          f"({current.status.code.name}: {current.status.message})",
                          self.state if self.state != "ready" else "not_persisted", self.unavailable().next_action)

    def persist(self, updates):
        """Persists entries {(label, story_id): Checkpoint} in one aggregate; state changes only after Durable."""
        with self.lock:
            self.ensure_ready()
            entries = dict(self.entries)
            for (label, story_id), value in updates.items():
                entries[entry_key(label, story_id)] = value
            self._append(entries)
            self.entries = entries
            self._save_hints()
            return self.last_id

    def _save_hints(self):
        hints = {"control": [str(w.writer_id), str(w.incarnation)] if (w := self.session.status().writer) else None,
                 "entries": {key: [str(v.writer.writer_id), str(v.writer.incarnation)]
                             for key, v in self.entries.items() if v.writer}}
        try:
            self.launcher.save_hints(hints)
        except OSError:
            pass

    def resume_for(self, label, ref):
        """The latest record for (label, context), or a recovery record when the prior state is unknown."""
        with self.lock:
            entry = self.entries.get(entry_key(label, ref.story_id))
            if entry is not None or self.prior_unknown_below is None:
                return entry
            identity = cl.AgentIdentity(self.launcher.base, label)
            resume = cl.Checkpoint(identity, ref, prior_state_unknown_below=self.prior_unknown_below)
            hint = self.launcher.hints().get("entries", {}).get(entry_key(label, ref.story_id))
            if hint:
                resume = replace(resume, writer=cl.WriterStamp(int(hint[0]), int(hint[1])),
                                 acquisition=self.launcher.provenance())
            return resume

    def read_checkpoint(self, checkpoint_id, label, story_id):
        """The entry for (label, story) in the exact aggregate a checkpoint_id names."""
        at = position_of(untoken("c1", checkpoint_id))
        if at.id.story_id != self.ref.story_id:
            raise ValueError("checkpoint_id names another checkpoint store")
        page = self.reader.recall(options=cl.RecallOptions(start=at.hlc, end=successor(at.hlc)), timeout=self.timeout)
        for event in page.events:
            if event.id == at.id:
                for item in json.loads(event.envelope.payload)["entries"]:
                    if item["label"] == label and item["checkpoint"]["context"][0] == story_id:
                        return cl.decode_checkpoint(compact(item["checkpoint"]).encode())
                raise ValueError("checkpoint_id holds no entry for this agent label and context")
        raise ValueError("checkpoint_id not readable"
                         + ("" if page.answer_complete else " (incomplete Replay; retry)"))

    def admit(self, label, story_id, checkpoint, operation_id, writable_sessions):
        """RESOURCE_EXHAUSTED before dispatch when the operation's worst-case record would not fit."""
        with self.lock:
            entries = dict(self.entries)
            stamp = checkpoint.writer or cl.WriterStamp(2**64 - 1, 2**64 - 1)
            entries[entry_key(label, story_id)] = replace(
                checkpoint, permanently_unknown_operations=checkpoint.permanently_unknown_operations + (
                    cl.UnknownOperation(operation_id, (stamp,), cl.HlcRange(MAX_HLC, MAX_HLC), True,
                                        b"\xff" * 32),))
            needed = self.encoded_size(entries) + self.reserve(writable_sessions)
            if needed > self.max_payload:
                raise Unavailable(f"RESOURCE_EXHAUSTED: checkpoint capacity {self.max_payload} bytes cannot hold "
                                  f"{needed} bytes for this operation and the lifecycle reserve", "resource_exhausted")

    def admit_session(self, label, ref, writable_sessions):
        sample = cl.Checkpoint(cl.AgentIdentity(self.launcher.base, label), ref, causal_floor=MAX_HLC,
                               writer=cl.WriterStamp(2**64 - 1, 2**64 - 1),
                               acquisition=cl.AcquisitionProvenance(self.launcher.host_id, self.launcher.lock_id,
                                                                    2**64 - 1, MAX_HLC),
                               last_own_receipt_hlc=MAX_HLC)
        with self.lock:
            entries = dict(self.entries)
            key = entry_key(label, ref.story_id)
            entries[key] = entries.get(key) or sample
            needed = self.encoded_size(entries) + self.reserve(writable_sessions + 1)
            if needed > self.max_payload:
                raise Unavailable(f"RESOURCE_EXHAUSTED: checkpoint capacity {self.max_payload} bytes cannot hold "
                                  f"{needed} bytes for another writable session", "resource_exhausted")

    def status(self, writable_sessions):
        with self.lock:
            writer = self.session.status().writer if self.session else None
            return {"state": self.state, "verdict": self.verdict,
                    "story": None if self.ref is None else {"story_id": str(self.ref.story_id),
                                                            "chronicle": self.ref.chronicle, "name": self.ref.name},
                    "control_writer": None if writer is None else {"writer_id": str(writer.writer_id),
                                                                   "incarnation": str(writer.incarnation)},
                    "checkpoint_id": self.last_id,
                    "prior_state_unknown_below": hlc_json(self.prior_unknown_below),
                    "encoded_bytes": self.encoded_size(self.entries) if self.state == "ready" else None,
                    "reserve_bytes_per_session": self.reserve(1), "writable_sessions": writable_sessions,
                    "max_payload_bytes": self.max_payload}

    def close(self):
        with self.lock:
            if self.session is not None and self.state == "ready":
                try:
                    self.session.close(timeout=self.timeout)
                except cl.Error:
                    pass
                self.state, self.verdict = "closed", "checkpoint store closed; the next writable tool reopens it"
