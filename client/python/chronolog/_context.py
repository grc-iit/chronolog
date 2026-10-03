"""Context API values and handles over the native Context layer. Ids and nanoseconds are exact ints, payloads and
trace fields bytes, timeouts seconds. The binding adds no routing or retry state machine of its own."""
from dataclasses import dataclass, field
from enum import IntEnum

from . import _core
from . import (Durability, Envelope, Event, Completion, Hlc, HlcRange, Position, Status, TimeReading, AppendResult,
               _Value, _raise)

__all__ = [
    "Access", "SessionState", "MemoryOutcome", "DeliveryLimit", "ReconcileOutcome", "FollowFrom", "ContextRef",
    "AgentIdentity", "WriterStamp", "PageLimits", "Memory", "PriorOutcome", "MemoryResult", "RememberOptions", "Page",
    "RecallOptions", "LatestOptions", "LatestResult", "AcquisitionProvenance", "RecoveryIncarnation",
    "ReconcileCheckpoint", "ReconciledOperation", "OperationDisposition", "UnknownOperation", "Checkpoint",
    "OpenOptions", "ReconcileOptions", "ReconcileResult", "CloseResult", "SessionStatus", "ContextOptions",
    "FollowInput", "FollowOptions", "FollowPage", "FollowResult", "ContextClient", "ContextSession",
    "connect_context", "encode_checkpoint", "decode_checkpoint",
]


class Access(IntEnum):
    READ_ONLY = 0
    READ_WRITE = 1


class SessionState(IntEnum):
    READY = 0
    TRANSPORT_PENDING = 1
    NEEDS_RECONCILE = 2
    FENCED = 3
    CLOSED = 4


class MemoryOutcome(IntEnum):
    DURABLE = 0
    RAM_ONLY_MAY_VANISH = 1
    REJECTED = 2
    UNKNOWN = 3
    FENCED = 4
    LANDED = 5


class DeliveryLimit(IntEnum):
    NONE = 0
    EVENTS = 1
    BYTES = 2
    OVERSIZED_EVENT = 3
    READ_CALLS = 4


class ReconcileOutcome(IntEnum):
    LANDED = 0
    ABSENT = 1
    UNKNOWN = 2


class FollowFrom(IntEnum):
    NOW = 0
    BEGINNING = 1
    POSITION = 2


@dataclass(frozen=True)
class ContextRef:
    story_id: int
    chronicle: str
    name: str


@dataclass(frozen=True)
class AgentIdentity:
    """control selects the reserved checkpoint-store writer kind, encoded under agent-context-control/v2:, which
    no user identity can produce."""
    agent_id: str
    slot: str
    control: bool = False


@dataclass(frozen=True)
class WriterStamp:
    writer_id: int
    incarnation: int


@dataclass(frozen=True)
class PageLimits:
    """None keeps the native default (1000 events, 256 KiB)."""
    max_events: int | None = None
    max_raw_bytes: int | None = None


@dataclass(frozen=True)
class Memory(_Value):
    operation_id: str
    envelope: Envelope
    durability: Durability = Durability.DURABLE
    physical: TimeReading | None = None


@dataclass(frozen=True)
class PriorOutcome(_Value):
    operation_id: str
    status: Status
    outcome: MemoryOutcome
    receipt: AppendResult | None = None
    landed: Position | None = None
    observed_durability: Durability = Durability.UNSPECIFIED


@dataclass(frozen=True)
class MemoryResult(_Value):
    current: PriorOutcome
    resolved_prior: tuple[PriorOutcome, ...] = ()
    blocking_operation_id: str | None = None
    state: SessionState = SessionState.READY


@dataclass(frozen=True)
class RememberOptions:
    resend_after_absent: bool = False


@dataclass(frozen=True)
class Page(_Value):
    events: tuple[Event, ...]
    range: HlcRange | None
    completion: Completion | None
    completion_range: HlcRange | None
    stream_status: Status
    limited: DeliveryLimit
    raw_bytes: int
    answer_complete: bool
    has_more: bool
    idle: bool
    cut_covers_causal_floor: bool
    after: Position | None
    next_cursor: str | None
    delivered_prefix_end: Hlc | None


@dataclass(frozen=True)
class RecallOptions:
    start: Hlc | None = None
    end: Hlc | None = None
    cursor: str | None = None
    limits: PageLimits | None = None
    max_read_calls: int | None = None


@dataclass(frozen=True)
class LatestOptions:
    before: Hlc | None = None
    limits: PageLimits | None = None
    max_read_calls: int | None = None


@dataclass(frozen=True)
class LatestResult:
    page: Page
    as_of: Hlc | None
    selection_complete: bool


@dataclass(frozen=True)
class AcquisitionProvenance:
    host_id: str
    launcher_lock_id: str
    expected_prior_incarnation: int | None = None
    acquisition_record_receipt_hlc: Hlc | None = None


@dataclass(frozen=True)
class RecoveryIncarnation:
    writer: WriterStamp
    marker_operation_id: str | None = None


@dataclass(frozen=True)
class ReconcileCheckpoint(_Value):
    transition_id: str
    lower_bound: Hlc
    recovered_incarnations: tuple[RecoveryIncarnation, ...] = ()
    current_marker_operation_id: str | None = None
    marker_hlc: Hlc | None = None


@dataclass(frozen=True)
class ReconciledOperation(_Value):
    operation_id: str
    outcome: ReconcileOutcome
    landed: Position | None = None
    observed_durability: Durability = Durability.UNSPECIFIED


@dataclass(frozen=True)
class OperationDisposition:
    result: ReconciledOperation
    prior_writer: WriterStamp
    normalized_digest: bytes | None = None


@dataclass(frozen=True)
class UnknownOperation(_Value):
    operation_id: str
    incarnations: tuple[WriterStamp, ...] = ()
    window: HlcRange | None = None
    absence_provable: bool = False
    normalized_digest: bytes | None = None


@dataclass(frozen=True)
class Checkpoint(_Value):
    """A value snapshot of a session; it never carries an acquire request id."""
    identity: AgentIdentity
    context: ContextRef
    causal_floor: Hlc = Hlc()
    processed_after: Position | None = None
    writer: WriterStamp | None = None
    acquisition: AcquisitionProvenance | None = None
    last_own_receipt_hlc: Hlc | None = None
    recovery: ReconcileCheckpoint | None = None
    unresolved_operations: tuple[str, ...] = ()
    permanently_unknown_operations: tuple[UnknownOperation, ...] = ()
    dispositions: tuple[OperationDisposition, ...] = ()
    prior_state_unknown_below: Hlc | None = None
    acquisition_closed: bool = False
    reconcile_attempted: bool = False
    takeover_required: bool = False


@dataclass(frozen=True)
class OpenOptions(_Value):
    access: Access = Access.READ_WRITE
    session_id: str = ""
    resume: Checkpoint | None = None
    ownership: AcquisitionProvenance | None = None


@dataclass(frozen=True)
class ReconcileOptions(_Value):
    operation_ids: tuple[str, ...] = ()
    takeover: bool = False
    max_read_calls: int | None = None


@dataclass(frozen=True)
class ReconcileResult(_Value):
    writer: WriterStamp | None
    marker_hlc: Hlc | None
    range: HlcRange | None
    operations: tuple[ReconciledOperation, ...]
    completion: Completion | None
    status: Status
    attempted: bool
    permanently_unknown_operations: tuple[str, ...]
    proof_complete: bool
    supply_all_unseen_operation_ids: bool
    omitted_operation_ids_may_duplicate: bool


@dataclass(frozen=True)
class CloseResult:
    release_committed: bool
    fenced: bool


@dataclass(frozen=True)
class SessionStatus(_Value):
    context: ContextRef
    identity: AgentIdentity
    writer: WriterStamp | None
    state: SessionState
    blocking_operation_id: str | None
    unresolved_operations: tuple[str, ...]
    permanently_unknown_operations: tuple[str, ...]
    reconcile_attempted: bool
    causal_floor: Hlc


@dataclass(frozen=True)
class ContextOptions:
    """SDK connection settings plus Context bounds; a None bound keeps the native default."""
    catalog: str
    player: str | None = None
    timeout: float = 10.0
    max_retries: int = 3
    retry_backoff: float = 0.02
    max_in_flight: int = 4
    batch_size: int = 128
    max_batch_items: int = 10000
    max_batch_bytes: int = 64 << 20
    channel_args: dict | None = field(default=None, hash=False)
    max_writable_sessions: int | None = None
    max_pending_operation_bytes: int | None = None
    max_completed_operations: int | None = None
    max_unresolved_operations: int | None = None
    max_persisted_dispositions: int | None = None
    max_checkpoint_payload_bytes: int | None = None
    cut_probe_width_ns: int | None = None


@dataclass(frozen=True)
class FollowInput(_Value):
    session: "ContextSession"
    from_: FollowFrom = FollowFrom.NOW
    after: Position | None = None


@dataclass(frozen=True)
class FollowOptions:
    """wait bounds an idle poll in seconds; the native wait has whole-second granularity, so it rounds up."""
    limits: PageLimits | None = None
    wait: float | None = None


@dataclass(frozen=True)
class FollowPage(_Value):
    context: ContextRef
    page: Page
    resume: Position | None
    starting_cut: Hlc | None
    uncertified_route_keepers: tuple[str, ...]


@dataclass(frozen=True)
class FollowResult(_Value):
    pages: tuple[FollowPage, ...]
    status: Status
    idle: bool


class ContextSession:
    def __init__(self, handle):
        self._handle = handle
        self.context = handle.context()
        self.identity = handle.identity()

    def remember(self, memory, *, options=RememberOptions(), timeout=None):
        return self._handle.remember(memory, options, timeout)

    def recall(self, *, options=RecallOptions(), timeout=None):
        return self._handle.recall(options, timeout)

    def latest(self, n, *, options=LatestOptions(), timeout=None):
        return self._handle.latest(n, options, timeout)

    def latest_aggregate(self, content_type, *, options=LatestOptions(), timeout=None):
        """The newest event of this story with exactly this content type, at a verified cut: the checkpoint store's
        n=1 lookup. selection_complete is false when the bounded search could not prove it."""
        return self._handle.latest_aggregate(content_type, options, timeout)

    def reconcile(self, *, options=ReconcileOptions(), timeout=None):
        return self._handle.reconcile(options, timeout)

    def acknowledge_processed(self, position):
        self._handle.acknowledge_processed(position)

    def checkpoint(self):
        return self._handle.checkpoint()

    def status(self):
        return self._handle.status()

    def close(self, *, timeout=None):
        return self._handle.close(timeout)


class ContextClient:
    def __init__(self, handle):
        self._handle = handle

    def ensure_context(self, chronicle, name, *, timeout=None):
        return self._handle.ensure_context(chronicle, name, timeout)

    def list_contexts(self, chronicle, *, timeout=None):
        return self._handle.list_contexts(chronicle, timeout)

    def open(self, context, identity, *, options=OpenOptions(), timeout=None):
        return ContextSession(self._handle.open(context, identity, options, timeout))

    def follow(self, inputs, *, options=FollowOptions(), timeout=None):
        """One bounded poll over every input with a shared deadline."""
        return self._handle.follow(list(inputs), options, timeout)

    def subscribe(self, inputs, *, options=FollowOptions(), timeout=None):
        """Polls follow, yielding every result that is not idle and resuming each input after its last delivered
        Position. A failed poll raises its typed status with the partial FollowResult attached as `result`."""
        current = list(inputs)
        while True:
            result = self.follow(current, options=options, timeout=timeout)
            if not result.status.ok:
                _raise(result.status, result)
            current = [FollowInput(i.session, FollowFrom.POSITION, page.resume) if page.resume else i
                       for i, page in zip(current, result.pages)] + current[len(result.pages):]
            if not result.idle:
                yield result


def connect_context(options, *, timeout=None):
    return ContextClient(_core.connect_context(options, timeout))


def encode_checkpoint(checkpoint, max_bytes=None):
    """The native versioned encoding; RESOURCE_EXHAUSTED above max_bytes."""
    return _core.encode_checkpoint(checkpoint, max_bytes)


def decode_checkpoint(data):
    return _core.decode_checkpoint(data)
