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
            return candidate(value)
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

    def __init__(self, payload=b"", content_type=None, attributes=None, trace_id=None, span_id=None):
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
        object.__setattr__(self, "durability", Durability(self.durability))

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

    def __post_init__(self):
        object.__setattr__(self, "laggards", tuple(self.laggards))
        object.__setattr__(self, "reason", IncompleteReason(self.reason))


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
        object.__setattr__(self, "durability", Durability(self.durability))

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


class Writer:
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

    def append(self, payload, *, content_type=None, attributes=None, trace_id=None, span_id=None,
               durability=Durability.DURABLE, physical=None, timeout=None):
        envelope = Envelope(payload, content_type, attributes, trace_id, span_id)
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

    def new_acquire_request_id(self):
        """A random 128-bit id owned by this Client in this process, to retain one logical acquire across calls."""
        return self._handle.new_acquire_request_id()

    def read_physical(self, story, start, end, *, timeout=None):
        return ReadStream(self._handle.read_physical(_id(story), start, end, timeout), timeout)

    def read(self, story, start=None, end=None, *, timeout=None):
        handle = self._handle.read(_id(story), start or Hlc(), end or Hlc(2**63 - 1, 2**32 - 1), timeout)
        return ReadStream(handle, timeout)

    def tail(self, story, after=None, *, timeout=None):
        return TailStream(self._handle.tail(_id(story), after, timeout), timeout)

    def __enter__(self): return self
    def __exit__(self, *args): return False


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
