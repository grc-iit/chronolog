from collections import deque
from dataclasses import dataclass, field
from enum import IntEnum
import re
import threading


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


@dataclass(frozen=True)
class ItemStatus:
    code: int
    message: str
    expected_sequence: int | None = None


class Error(Exception):
    def __init__(self, status):
        self.status = status
        self.expected_sequence = status.expected_sequence
        super().__init__(f"{status.code}: {status.message}")


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


def _error(code, message):
    match = re.search(r"expected(?:[ _]sequence)?\s*[:=]?\s*(\d+)", message, re.I)
    status = ItemStatus(code, message, int(match[1]) if match else None)
    cls = {1: Cancelled, 3: InvalidArgument, 4: DeadlineExceeded, 5: NotFound,
           6: AlreadyExists, 8: ResourceExhausted, 9: FailedPrecondition,
           11: OutOfRange, 12: Unimplemented, 14: Unavailable}.get(code, Error)
    return cls(status)


from . import _core


def _name(chronicle):
    return chronicle.name if isinstance(chronicle, Chronicle) else chronicle


def _id(story):
    return story.id if isinstance(story, Story) else story


class Writer:
    def __init__(self, handle):
        self._handle = handle

    @property
    def writer_id(self): return self._handle.acquisition()["writer_id"]
    @property
    def incarnation(self): return self._handle.acquisition()["incarnation"]
    @property
    def story_id(self): return self._handle.acquisition()["story_id"]
    @property
    def route(self): return self._handle.acquisition()["route"]
    @property
    def assigned_keeper(self): return self._handle.acquisition()["assigned_keeper"]

    def append(self, payload, *, content_type=None, attributes=None, trace_id=None, span_id=None,
               durability=Durability.DURABLE, timeout=None):
        envelope = Envelope(payload, content_type, attributes, trace_id, span_id)
        return self._handle.append(AppendSpec(envelope, durability), timeout)

    def append_batch(self, items, *, durability=Durability.DURABLE, timeout=None):
        specs = []
        for item in items:
            if isinstance(item, AppendSpec):
                specs.append(item)
            elif isinstance(item, dict):
                fields = dict(item)
                level = fields.pop("durability", durability)
                specs.append(AppendSpec(Envelope(**fields), level))
            else:
                specs.append(AppendSpec(item if isinstance(item, Envelope) else Envelope(item), durability))
        return self._handle.append_batch(specs, timeout)

    def release(self, *, timeout=None):
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
    def acquire(self, story, identity, *, timeout=None): return Writer(self._handle.acquire(_id(story), identity, timeout))

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
    return Client(_core.connect(catalog, player or "", timeout, max_retries, retry_backoff,
                               max_in_flight, batch_size, max_batch_items, max_batch_bytes,
                               channel_args or {}))
