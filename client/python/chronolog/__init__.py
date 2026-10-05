from collections import deque
from dataclasses import dataclass, field, fields as _fields
from enum import IntEnum
import atexit
import re
import threading
import typing


class Durability(IntEnum):
    UNSPECIFIED = 0
    ACCEPTED = 1
    DURABLE = 2


class IncompleteReason(IntEnum):
    NONE = 0
    LAGGING_WRITERS = 1
    PHYSICAL_AXIS_UNBOUNDED = 2
    SOURCE_FAILED = 3
    TRUNCATED = 4


class StatusCode(IntEnum):
    OK = 0
    CANCELLED = 1
    UNKNOWN = 2
    INVALID_ARGUMENT = 3
    DEADLINE_EXCEEDED = 4
    NOT_FOUND = 5
    ALREADY_EXISTS = 6
    PERMISSION_DENIED = 7
    RESOURCE_EXHAUSTED = 8
    FAILED_PRECONDITION = 9
    ABORTED = 10
    OUT_OF_RANGE = 11
    UNIMPLEMENTED = 12
    INTERNAL = 13
    UNAVAILABLE = 14
    DATA_LOSS = 15
    UNAUTHENTICATED = 16


class AppendRejection(IntEnum):
    UNSPECIFIED = 0
    FENCED_RELEASED = 1
    FENCED_SUPERSEDED = 2
    SEQUENCE_GAP = 3
    DEDUPE_WINDOW = 4
    EARLIER_ITEM_FAILED = 5
    NOT_REGISTERED = 6
    STALE_EPOCH = 7
    UNASSIGNED_KEEPER = 8
    KEEPER_NOT_IN_ROUTE = 9
    STORY_TOMBSTONED = 10
    FENCED_EXPIRED = 11
    FENCED_OWNER_REMOVED = 12
    CAPACITY = 13


class AcquireRefusalReason(IntEnum):
    UNSPECIFIED = 0
    HELD = 1
    PRIOR_MISMATCH = 2


class AcquisitionTerminationCause(IntEnum):
    UNSPECIFIED = 0
    EXPIRED = 1
    RELEASED = 2
    SUPERSEDED = 3
    OWNER_REMOVED = 4


class AwaitAnswer(IntEnum):
    FOUND = 1
    ABSENT_THROUGH = 2
    CONSUMED = 3
    NEVER = 4
    UNKNOWN = 5


class KeeperPreferenceResult(IntEnum):
    UNSPECIFIED = 0
    HONORED = 1
    NOT_IN_ROUTE = 2
    RETAINED = 3


def _coerce(kind, value):
    if isinstance(value, list):
        return tuple(value)
    if value is None:
        return None
    for candidate in (kind, *typing.get_args(kind)):
        if isinstance(candidate, type) and issubclass(candidate, IntEnum) and type(value) is not candidate:
            try:
                return candidate(value)
            except ValueError:
                if isinstance(value, int):
                    return int(value)
                raise
    return value


class _Value:
    """Freezes sequences into tuples and native enum ints into their IntEnum."""

    def __post_init__(self):
        for f in _fields(self):
            current = getattr(self, f.name)
            coerced = _coerce(f.type, current)
            if coerced is not current:
                object.__setattr__(self, f.name, coerced)


@dataclass(frozen=True, order=True)
class Hlc:
    physical_ns: int = 0
    logical: int = 0


@dataclass(frozen=True)
class EventId:
    story_id: int = 0
    writer_id: int = 0
    incarnation: int = 0
    sequence: int = 0


@dataclass(frozen=True)
class Link:
    type: str
    target: EventId
    target_hlc: Hlc | None = None


@dataclass(frozen=True)
class TimeReading:
    physical_ns: int = 0
    uncertainty_ns: int | None = None
    status: int = 1


@dataclass(frozen=True)
class KeeperRef:
    process_id: str
    endpoint: str


@dataclass(frozen=True)
class Route:
    epoch: int = 0
    keepers: tuple[KeeperRef, ...] = ()
    grapher: str = ""
    player: str = ""

    def __post_init__(self):
        object.__setattr__(self, "keepers", tuple(self.keepers))


@dataclass(frozen=True, init=False)
class Envelope:
    payload: bytes
    content_type: str
    trace_id: bytes
    span_id: bytes
    _attributes: tuple[tuple[str, str], ...] = field(repr=False)
    kind: str
    actor: str
    links: tuple[Link, ...]

    def __init__(self, payload=b"", content_type=None, attributes=None, trace_id=None, span_id=None,
                 kind=None, actor=None, links=None):
        for name, data in (("payload", payload), ("trace_id", trace_id or b""), ("span_id", span_id or b"")):
            if not isinstance(data, bytes):
                raise TypeError(f"{name} must be bytes")
            object.__setattr__(self, name, data)
        if content_type is not None and not isinstance(content_type, str):
            raise TypeError("content_type must be str")
        attrs = dict(attributes or {})
        if any(not isinstance(k, str) or not isinstance(v, str) for k, v in attrs.items()):
            raise TypeError("attributes must be dict[str, str]")
        object.__setattr__(self, "content_type", content_type or "")
        object.__setattr__(self, "_attributes", tuple(sorted(attrs.items())))
        for name, value in (("kind", kind), ("actor", actor)):
            if value is not None and not isinstance(value, str):
                raise TypeError(f"{name} must be str")
            object.__setattr__(self, name, value or "")
        links = tuple(links or ())
        if any(not isinstance(link, Link) for link in links):
            raise TypeError("links must be Link")
        object.__setattr__(self, "links", links)

    @property
    def attributes(self):
        return dict(self._attributes)


@dataclass(frozen=True)
class Event:
    id: EventId
    physical: TimeReading
    hlc: Hlc
    envelope: Envelope
    durability: Durability

    def __post_init__(self):
        object.__setattr__(self, "durability", _coerce(Durability, self.durability))

    @property
    def payload(self):
        return self.envelope.payload


@dataclass(frozen=True)
class Frontier:
    writer_id: int
    incarnation: int
    frontier: Hlc


@dataclass(frozen=True)
class Completion:
    complete: bool = False
    frontier: Hlc = Hlc()
    laggards: tuple[Frontier, ...] = ()
    reason: IncompleteReason = IncompleteReason.NONE
    # Newest-first reads only. claim_end is the end e of the Read. claim_start is c of a TRUNCATED Read: the events at or
    # above c are complete on their own, and the next read covers [start, claim_start).
    claim_start: typing.Optional[Hlc] = None
    claim_end: typing.Optional[Hlc] = None

    def __post_init__(self):
        object.__setattr__(self, "laggards", tuple(self.laggards))
        object.__setattr__(self, "reason", _coerce(IncompleteReason, self.reason))


@dataclass(frozen=True)
class AwaitResult:
    """FOUND carries event; ABSENT_THROUGH carries the exclusive frontier through which the event is certified absent."""
    answer: AwaitAnswer
    event: typing.Optional[Event] = None
    frontier: typing.Optional[Hlc] = None

    def __post_init__(self):
        object.__setattr__(self, "answer", _coerce(AwaitAnswer, self.answer))


@dataclass(frozen=True)
class Chronicle:
    name: str
    tombstoned: bool = False


@dataclass(frozen=True)
class Story:
    id: int
    chronicle: str
    name: str
    epoch: int = 1
    tombstoned: bool = False

    @property
    def story_id(self):
        return self.id


@dataclass(frozen=True)
class AppendResult:
    event_id: EventId
    hlc: Hlc
    durability: Durability

    def __post_init__(self):
        object.__setattr__(self, "durability", _coerce(Durability, self.durability))

    @property
    def acked(self):
        return self.durability == Durability.DURABLE


@dataclass(frozen=True)
class AppendSpec:
    envelope: Envelope
    durability: Durability = Durability.DURABLE
    physical: TimeReading | None = None


@dataclass(frozen=True)
class AcquireRefusal(_Value):
    refusal_reason: AcquireRefusalReason = AcquireRefusalReason.UNSPECIFIED
    current_incarnation: int | None = None
    matched_incarnation: int | None = None
    remaining_ns: int = 0
    termination_cause: AcquisitionTerminationCause | None = None


@dataclass(frozen=True)
class Status(_Value):
    """A native status: its code, message and the typed details read from its payload, never from the text."""
    code: StatusCode = StatusCode.OK
    message: str = ""
    expected_sequence: int | None = None
    rejection: AppendRejection = AppendRejection.UNSPECIFIED
    acquire_refusal: AcquireRefusal | None = None

    @property
    def ok(self):
        return self.code == StatusCode.OK


ItemStatus = Status


class Error(Exception):
    def __init__(self, status):
        self.status = status
        self.expected_sequence = status.expected_sequence
        super().__init__(f"{status.code}: {status.message}")

    @property
    def code(self): return self.status.code
    @property
    def rejection(self): return self.status.rejection
    @property
    def acquire_refusal(self): return self.status.acquire_refusal


class Unavailable(Error): pass
class FailedPrecondition(Error): pass
class InvalidArgument(Error): pass
class OutOfRange(Error): pass
class Unimplemented(Error): pass
class NotFound(Error): pass
class Cancelled(Error): pass
class DeadlineExceeded(Error): pass
class AlreadyExists(Error): pass
class ResourceExhausted(Error): pass
class PermissionDenied(Error): pass
class Aborted(Error): pass
class Internal(Error): pass
class DataLoss(Error): pass
class Unauthenticated(Error): pass

_errors = {1: Cancelled, 3: InvalidArgument, 4: DeadlineExceeded, 5: NotFound, 6: AlreadyExists,
           7: PermissionDenied, 8: ResourceExhausted, 9: FailedPrecondition, 10: Aborted, 11: OutOfRange,
           12: Unimplemented, 13: Internal, 14: Unavailable, 15: DataLoss, 16: Unauthenticated}


def _status(code, message, rejection=0, acquire_refusal=None):
    match = re.search(r"expected(?:[ _]sequence)?\s*[:=]?\s*(\d+)", message, re.I)
    return Status(code, message, int(match[1]) if match else None, rejection, acquire_refusal)


def _error(code, message, rejection=0, acquire_refusal=None):
    return _errors.get(code, Error)(_status(code, message, rejection, acquire_refusal))


def _raise(status, result):
    error = _errors.get(status.code, Error)(status)
    error.result = result
    raise error


def rejection_of(status):
    """The typed append rejection a Status or Error carries; UNSPECIFIED when it carries none."""
    if isinstance(status, Error):
        status = status.status
    return status.rejection


def acquire_refusal_of(status):
    """The typed HELD, PRIOR_MISMATCH or terminal-retry detail of a refused acquire, or None."""
    if isinstance(status, Error):
        status = status.status
    return status.acquire_refusal


@dataclass(frozen=True)
class Position:
    hlc: Hlc
    id: EventId


@dataclass(frozen=True)
class HlcRange:
    start: Hlc
    end: Hlc


@dataclass(frozen=True)
class AcquisitionLease:
    duration_ns: int = 0
    remaining_ns: int = 0


@dataclass(frozen=True)
class Acquisition(_Value):
    story_id: int
    writer_id: int
    incarnation: int
    route: Route
    assigned_keeper: KeeperRef
    lease: AcquisitionLease
    keeper_preference: KeeperPreferenceResult | None = None


@dataclass(frozen=True)
class WriterLease(_Value):
    grant: AcquisitionLease
    estimated_remaining_ns: int
    # Diagnostic only: dispatch never stops on it.
    confirmed: bool
    termination_cause: AcquisitionTerminationCause | None
    renewals: int


@dataclass(frozen=True)
class AcquireOptions:
    """Thin native AcquireOptions. acquire_request_id must come from new_acquire_request_id on the same Client in
    this process; None reuses an identical unresolved acquire's id or mints one before dispatch."""
    lease_duration_ns: int | None = None
    preferred_keeper_process_id: str | None = None
    takeover: bool = False
    expected_prior_incarnation: int | None = None
    acquire_request_id: str | None = None


from . import _core


def _name(chronicle):
    return chronicle.name if isinstance(chronicle, Chronicle) else chronicle


def _id(story):
    return story.id if isinstance(story, Story) else story


class _Appender:
    def append(self, payload, *, content_type=None, attributes=None, trace_id=None, span_id=None,
               kind=None, actor=None, links=None, durability=Durability.DURABLE, physical=None, timeout=None):
        envelope = Envelope(payload, content_type, attributes, trace_id, span_id, kind, actor, links)
        return self._handle.append(AppendSpec(envelope, durability, physical), timeout)

    def append_batch(self, items, *, durability=Durability.DURABLE, timeout=None):
        specs = []
        for item in items:
            if isinstance(item, AppendSpec):
                specs.append(item)
            elif isinstance(item, dict):
                fields = dict(item)
                level = fields.pop("durability", durability)
                physical = fields.pop("physical", None)
                specs.append(AppendSpec(Envelope(**fields), level, physical))
            else:
                specs.append(AppendSpec(item if isinstance(item, Envelope) else Envelope(item), durability))
        return self._handle.append_batch(specs, timeout)

    def release(self, *, timeout=None):
        """Stops renewal before sending Release."""
        return self._handle.release(timeout)

    def __enter__(self): return self

    def __exit__(self, typ, exc, tb):
        try:
            self.release()
        except Error:
            if exc is None:
                raise
        return False


class Writer(_Appender):
    def __init__(self, handle):
        self._handle = handle
        self.acquisition = handle.acquisition()

    @property
    def writer_id(self): return self.acquisition.writer_id
    @property
    def incarnation(self): return self.acquisition.incarnation
    @property
    def story_id(self): return self.acquisition.story_id
    @property
    def route(self): return self.acquisition.route
    @property
    def assigned_keeper(self): return self.acquisition.assigned_keeper

    def lease(self):
        """The last Catalog-confirmed grant and the local renewal estimate."""
        return self._handle.lease()


class LaneWriter(_Appender):
    """One logical writer spread across Keepers by time. Lane i is a Writer named identity + "/lane" + i; the lane of
    an append is (physical_ns // slice_ns) % lanes. A spec without a physical reading is stamped once before its
    first send. release stops every lane and returns whether each reported a release."""
    def __init__(self, handle):
        self._handle = handle
        self.lanes = handle.lanes()


@dataclass(frozen=True)
class _WirePredicate:
    kinds: tuple[str, ...]
    actors: tuple[str, ...]
    attributes: tuple[tuple[str, str], ...]
    links: tuple[tuple[str, EventId], ...]
    event_ids: tuple[EventId, ...]


_PREDICATE_KEYS = frozenset(("kinds", "actors", "attributes", "link_to", "event_ids"))


def _predicate(where):
    """Normalizes a predicate dict: kinds and actors are any-member sets of str, attributes maps key to value, link_to
    lists the EventIds or Links the event must link to (a bare EventId matches any link type) and event_ids is an
    any-member set of EventIds. All terms must hold. None matches every event."""
    if where is None:
        return None
    unknown = set(where) - _PREDICATE_KEYS
    if unknown:
        raise ValueError(f"unknown predicate keys: {sorted(unknown)}")

    def strings(value):
        return (value,) if isinstance(value, str) else tuple(value or ())

    attributes = where.get("attributes") or {}
    attributes = attributes.items() if hasattr(attributes, "items") else attributes
    links = tuple((i.type, i.target) if isinstance(i, Link) else ("", i) for i in where.get("link_to") or ())
    return _WirePredicate(strings(where.get("kinds")), strings(where.get("actors")),
                          tuple((k, v) for k, v in attributes), links, tuple(where.get("event_ids") or ()))


class ReadStream:
    def __init__(self, handle, timeout=None):
        self._handle = handle
        self._timeout = timeout
        self._events = deque()
        self._done = threading.Event()
        self._pull_lock = threading.Lock()
        self.completion = None
        self.continuation = None

    def __iter__(self): return self

    def __next__(self):
        with self._pull_lock:
            while not self._done.is_set():
                if self._events:
                    return self._events.popleft()
                try:
                    batch = self._handle.next(self._timeout)
                except Cancelled:
                    if self._done.is_set():
                        break
                    raise
                if batch is None:
                    self._done.set()
                    break
                events, completion, continuation = batch
                self._events.extend(events)
                if completion is not None:
                    self.completion = completion
                    self.continuation = continuation
                    self._done.set()
                    if self._events:
                        return self._events.popleft()
            raise StopIteration

    def cancel(self):
        self._done.set()
        self._handle.cancel()

    def __enter__(self): return self
    def __exit__(self, *args): self.cancel()


class TailStream(ReadStream): pass


class Client:
    def __init__(self, handle):
        self._handle = handle

    def create_chronicle(self, name, *, timeout=None): return self._handle.create_chronicle(name, timeout)
    def chronicle(self, name, *, timeout=None): return self._handle.chronicle(_name(name), timeout)
    def list_chronicles(self, *, timeout=None): return self._handle.list_chronicles(timeout)
    def destroy_chronicle(self, chronicle, *, timeout=None): return self._handle.destroy_chronicle(_name(chronicle), timeout)
    def create_story(self, chronicle, name, *, timeout=None): return self._handle.create_story(_name(chronicle), name, timeout)
    def story(self, story, *, timeout=None): return self._handle.story(_id(story), timeout)
    def list_stories(self, chronicle, *, timeout=None): return self._handle.list_stories(_name(chronicle), timeout)
    def destroy_story(self, story, *, timeout=None): return self._handle.destroy_story(_id(story), timeout)
    def acquire(self, story, identity, *, options=None, timeout=None):
        return Writer(self._handle.acquire(_id(story), identity, options, timeout))

    def acquire_lanes(self, story, identity, lanes, slice_ns, *, options=None, timeout=None):
        """Acquires min(lanes, route keepers) writers, lane i preferring keeper i. options.acquire_request_id must be
        None because each lane mints its own."""
        return LaneWriter(self._handle.acquire_lanes(_id(story), identity, lanes, slice_ns, options, timeout))

    def new_acquire_request_id(self):
        """A random 128-bit id owned by this Client in this process, to retain one logical acquire across calls."""
        return self._handle.new_acquire_request_id()

    def await_event(self, ref, *, hlc=None, bound_s=0.0, timeout=None):
        """Waits up to bound_s seconds for ref, an EventId. hlc is the event's hlc when the caller holds one. Returns
        an AwaitResult: FOUND, ABSENT_THROUGH, CONSUMED, NEVER or UNKNOWN."""
        return self._handle.await_event(ref, hlc, bound_s, timeout)

    def read_physical(self, story, start, end, *, timeout=None):
        return ReadStream(self._handle.read_physical(_id(story), start, end, timeout), timeout)

    def read(self, story=None, start=None, end=None, *, prefix=None, predicate=None, newest_first=False, max_events=None,
             timeout=None):
        """Reads one story, or with prefix the merged history of every story under that path prefix. predicate is a
        dict of kinds, actors, attributes, link_to and event_ids that keeps only matching events. newest_first returns
        the newest events of [start, end) in descending order, and an end left open means the minimum sealed frontier.
        A truncated newest-first read names completion.claim_start: continue with end=claim_start. max_events is a soft
        target of events to return, and zero or None leaves the Player's limit."""
        if (story is None) == (prefix is None):
            raise ValueError("pass exactly one of story or prefix")
        start, end = start or Hlc(), end or Hlc(2**63 - 1, 2**32 - 1)
        where = _predicate(predicate)
        if prefix is None:
            handle = self._handle.read(_id(story), start, end, where, newest_first, max_events, timeout)
        else:
            handle = self._handle.read_prefix(prefix, start, end, where, newest_first, max_events, timeout)
        return ReadStream(handle, timeout)

    def tail(self, story=None, after=None, *, prefix=None, predicate=None, timeout=None):
        """Follows one story, or with prefix every story under that path prefix; see read for predicate."""
        if (story is None) == (prefix is None):
            raise ValueError("pass exactly one of story or prefix")
        where = _predicate(predicate)
        if prefix is None:
            handle = self._handle.tail(_id(story), after, where, timeout)
        else:
            handle = self._handle.tail_prefix(prefix, after, where, timeout)
        return TailStream(handle, timeout)

    def scope(self, prefix):
        return Scope(self, prefix)

    def __enter__(self): return self
    def __exit__(self, *args): return False


class Scope:
    """A scope is a story path prefix such as "chronicle" or "chronicle/story": it names every story whose path equals
    the prefix or lies below it by whole segments. The methods are Client.read and Client.tail with the prefix fixed."""

    def __init__(self, client, prefix):
        self._client = client
        self.prefix = prefix

    def read(self, since=None, until=None, where=None, *, timeout=None):
        return self._client.read(prefix=self.prefix, start=since, end=until, predicate=where, timeout=timeout)

    def tail(self, after=None, where=None, *, timeout=None):
        return self._client.tail(prefix=self.prefix, after=after, predicate=where, timeout=timeout)

    def stories(self, *, timeout=None):
        return self._client._handle.list_stories_by_prefix(self.prefix, timeout)


def connect(catalog, player=None, *, timeout=10.0, max_retries=3, retry_backoff=0.02,
            max_in_flight=4, batch_size=128, max_batch_items=10000,
            max_batch_bytes=64 << 20, channel_args=None):
    return Client(_core.connect(ContextOptions(catalog, player, timeout=timeout, max_retries=max_retries,
                                               retry_backoff=retry_backoff, max_in_flight=max_in_flight,
                                               batch_size=batch_size, max_batch_items=max_batch_items,
                                               max_batch_bytes=max_batch_bytes, channel_args=channel_args)))


from ._context import *  # noqa: E402,F401,F403
from ._context import ContextOptions  # noqa: E402

# Interpreter exit refuses new native calls, cancels live streams, waits for in-flight calls and reaps handles.
atexit.register(_core._shutdown)
