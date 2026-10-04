import { join } from 'node:path';

export type UnknownEnum = `UNKNOWN_${number}`;
export enum Durability { UNSPECIFIED = 0, ACCEPTED = 1, DURABLE = 2 }
export type StatusCode = 'OK' | 'CANCELLED' | 'UNKNOWN' | 'INVALID_ARGUMENT' | 'DEADLINE_EXCEEDED' |
  'NOT_FOUND' | 'ALREADY_EXISTS' | 'PERMISSION_DENIED' | 'RESOURCE_EXHAUSTED' | 'FAILED_PRECONDITION' |
  'ABORTED' | 'OUT_OF_RANGE' | 'UNIMPLEMENTED' | 'INTERNAL' | 'UNAVAILABLE' | 'DATA_LOSS' | 'UNAUTHENTICATED' | UnknownEnum;
export interface ItemStatus { readonly code: number; readonly message: string }
export type AppendRejection = 'UNSPECIFIED' | 'FENCED_RELEASED' | 'FENCED_SUPERSEDED' | 'SEQUENCE_GAP' | 'DEDUPE_WINDOW' |
  'EARLIER_ITEM_FAILED' | 'NOT_REGISTERED' | 'STALE_EPOCH' | 'UNASSIGNED_KEEPER' | 'KEEPER_NOT_IN_ROUTE' |
  'STORY_TOMBSTONED' | 'FENCED_EXPIRED' | 'FENCED_OWNER_REMOVED' | 'CAPACITY' | UnknownEnum;
export type AcquisitionTerminationCause = 'UNSPECIFIED' | 'EXPIRED' | 'RELEASED' | 'SUPERSEDED' | 'OWNER_REMOVED' | UnknownEnum;
export interface AcquireRefusal {
  readonly refusalReason: 'UNSPECIFIED' | 'HELD' | 'PRIOR_MISMATCH' | UnknownEnum;
  readonly currentIncarnation?: bigint;
  readonly matchedIncarnation?: bigint;
  readonly remainingNs: bigint;
  readonly terminationCause?: AcquisitionTerminationCause;
}
// A native status as values and errors carry it: the code, message and typed details read from its payload.
export interface Status {
  readonly code: StatusCode;
  readonly message: string;
  readonly rejection: AppendRejection;
  readonly acquireRefusal?: AcquireRefusal;
}
export interface StatusDetail { readonly rejection?: AppendRejection; readonly acquireRefusal?: AcquireRefusal }
export class ChronologError extends Error implements Status {
  readonly itemStatus: ItemStatus;
  readonly rejection: AppendRejection;
  readonly acquireRefusal?: AcquireRefusal;
  constructor(readonly code: StatusCode, message: string, itemStatus: ItemStatus, detail: StatusDetail = {}) {
    super(message);
    this.name = new.target.name;
    this.itemStatus = Object.freeze({ ...itemStatus });
    this.rejection = detail.rejection ?? 'UNSPECIFIED';
    if (detail.acquireRefusal) this.acquireRefusal = freeze({ ...detail.acquireRefusal });
  }
}
export class Unavailable extends ChronologError {}
export class FailedPrecondition extends ChronologError {}
export class InvalidArgument extends ChronologError {}
export class OutOfRange extends ChronologError {}
export class Unimplemented extends ChronologError {}
export class NotFound extends ChronologError {}
export class Cancelled extends ChronologError {}
export class DeadlineExceeded extends ChronologError {}
export class AlreadyExists extends ChronologError {}
export class PermissionDenied extends ChronologError {}
export class ResourceExhausted extends ChronologError {}
export class Aborted extends ChronologError {}
export class Internal extends ChronologError {}
export class DataLoss extends ChronologError {}
export class Unauthenticated extends ChronologError {}
const errors: Partial<Record<StatusCode, typeof ChronologError>> = {
  UNAVAILABLE: Unavailable, FAILED_PRECONDITION: FailedPrecondition, INVALID_ARGUMENT: InvalidArgument,
  OUT_OF_RANGE: OutOfRange, UNIMPLEMENTED: Unimplemented, NOT_FOUND: NotFound, CANCELLED: Cancelled,
  DEADLINE_EXCEEDED: DeadlineExceeded, ALREADY_EXISTS: AlreadyExists, PERMISSION_DENIED: PermissionDenied,
  RESOURCE_EXHAUSTED: ResourceExhausted, ABORTED: Aborted, INTERNAL: Internal, DATA_LOSS: DataLoss,
  UNAUTHENTICATED: Unauthenticated,
};
function translate(error: unknown): Error {
  if (error instanceof ChronologError) return error;
  if (error && typeof error === 'object' && 'code' in error && 'itemStatus' in error) {
    const raw = error as { code: StatusCode; message: string; itemStatus: ItemStatus } & StatusDetail;
    return new (errors[raw.code] ?? ChronologError)(raw.code, raw.message, raw.itemStatus, raw);
  }
  return error instanceof Error ? error : new Error(String(error));
}
// The typed append rejection a status carries; UNSPECIFIED when it carries none.
export function rejectionOf(status: Status | Error): AppendRejection {
  return 'rejection' in status && status.rejection ? status.rejection : 'UNSPECIFIED';
}
// The typed HELD, PRIOR_MISMATCH or terminal-retry detail of a refused acquire, never parsed from the message.
export function acquireRefusalOf(status: Status | Error): AcquireRefusal | undefined {
  return 'acquireRefusal' in status ? status.acquireRefusal : undefined;
}
function cancelled(): Cancelled {
  return new Cancelled('CANCELLED', 'operation cancelled', { code: 1, message: 'operation cancelled' });
}
// An abort before dispatch sends nothing; an abort after dispatch stops waiting, and the native call ends on its own
// deadline with its result discarded.
async function invoke<T>(operation: () => Promise<T>, signal?: AbortSignal): Promise<T> {
  if (signal?.aborted) throw cancelled();
  let abort: (() => void) | undefined;
  try {
    const pending = operation();
    if (!signal) return await pending;
    void pending.catch(() => {});
    return await new Promise<T>((resolve, reject) => {
      abort = () => reject(cancelled());
      signal.addEventListener('abort', abort, { once: true });
      pending.then(resolve, reject);
    });
  } catch (error) { throw translate(error); }
  finally { if (abort) signal?.removeEventListener('abort', abort); }
}
function sync<T>(operation: () => T): T {
  try { return operation(); } catch (error) { throw translate(error); }
}
export interface Hlc { readonly physicalNs: bigint; readonly logical: number }
export interface EventId { readonly storyId: bigint; readonly writerId: bigint; readonly incarnation: bigint; readonly sequence: bigint }
export interface KeeperRef { readonly processId: string; readonly endpoint: string }
export interface Route { readonly epoch: bigint; readonly keepers: readonly KeeperRef[]; readonly grapher: string; readonly player: string }
export interface AcquisitionLease { readonly durationNs: bigint; readonly remainingNs: bigint }
export interface Acquisition {
  readonly storyId: bigint; readonly writerId: bigint; readonly incarnation: bigint; readonly route: Route; readonly assignedKeeper: KeeperRef;
  readonly lease: AcquisitionLease; readonly keeperPreference?: 'UNSPECIFIED' | 'HONORED' | 'NOT_IN_ROUTE' | 'RETAINED' | UnknownEnum;
}
export interface WriterLease {
  // Last Catalog-confirmed grant; Keeper admissions never refresh it.
  readonly grant: AcquisitionLease;
  readonly estimatedRemainingNs: bigint;
  // Diagnostic only: dispatch never stops on it.
  readonly confirmed: boolean;
  readonly terminationCause?: AcquisitionTerminationCause;
  readonly renewals: bigint;
}
export interface Envelope { readonly contentType: string; readonly payload: Uint8Array; readonly traceId: Uint8Array; readonly spanId: Uint8Array; readonly attributes: Readonly<Record<string, string>> }
export interface TimeReading { readonly physicalNs: bigint; readonly uncertaintyNs?: bigint; readonly status: 'SYNCED' | 'UNSYNCED' | 'UNAVAILABLE' | UnknownEnum }
export interface Event { readonly id: EventId; readonly hlc: Hlc; readonly physical: TimeReading; readonly envelope: Envelope; readonly durability: Durability | number }
export interface Frontier { readonly writerId: bigint; readonly incarnation: bigint; readonly frontier: Hlc }
export interface Completion { readonly complete: boolean; readonly frontier: Hlc; readonly laggards: readonly Frontier[]; readonly reason: 'NONE' | 'LAGGING_WRITERS' | 'PHYSICAL_AXIS_UNBOUNDED' | 'SOURCE_FAILED' | 'TRUNCATED' | UnknownEnum }
export interface Chronicle { readonly name: string; readonly tombstoned: boolean }
export interface Story { readonly id: bigint; readonly epoch: bigint; readonly chronicle: string; readonly name: string; readonly tombstoned: boolean }
export interface AppendResult { readonly eventId: EventId; readonly hlc: Hlc; readonly achieved: Durability | number; readonly acked: boolean }
export interface HlcRange { readonly start: Hlc; readonly end: Hlc }
export interface PhysicalRange { readonly startNs: bigint; readonly endNs: bigint }
export interface Position { readonly hlc: Hlc; readonly id: EventId }
export interface CallOptions { timeoutMs?: number; signal?: AbortSignal }
export interface AcquireOptions extends CallOptions {
  leaseDurationNs?: bigint; preferredKeeperProcessId?: string; takeover?: boolean; expectedPriorIncarnation?: bigint;
  // From newAcquireRequestId on the same Client in this process; omitted reuses or mints one before dispatch.
  acquireRequestId?: string;
}
export interface ConnectOptions extends CallOptions {
  catalog: string; player?: string; rpcTimeoutMs?: number;
  retry?: { maxRetries?: number; backoffMs?: number };
  channelArgs?: Record<string, string | number>;
  maxInFlight?: number; batchSize?: number; maxBatchItems?: number; maxBatchBytes?: number;
}
export interface AppendOptions extends CallOptions {
  contentType?: string; attributes?: Record<string, string>; traceId?: Uint8Array; spanId?: Uint8Array;
  durability?: Durability;
}
export interface AppendItem extends Omit<AppendOptions, 'timeoutMs'> { payload: Uint8Array }
export interface StreamOptions extends CallOptions {}
interface StreamItem { events: Event[]; completion?: Completion; continuation?: Hlc }
type Handle = object;
interface Core {
  connect(options: ConnectOptions): Promise<Handle>;
  catalog(handle: Handle, method: string, args: unknown[], options: CallOptions): Promise<unknown>;
  acquire(handle: Handle, story: bigint, identity: string, options: AcquireOptions): Promise<{ handle: Handle; acquisition: Acquisition }>;
  newAcquireRequestId(handle: Handle): string;
  lease(handle: Handle): WriterLease;
  append(handle: Handle, payload: Uint8Array, options: AppendOptions): Promise<AppendResult>;
  appendBatch(handle: Handle, items: readonly AppendItem[], options: CallOptions): Promise<(AppendResult | Error)[]>;
  release(handle: Handle, options: CallOptions): Promise<boolean>;
  stream(handle: Handle, story: bigint, mode: 'read' | 'tail' | 'physical', input: HlcRange | PhysicalRange | Position | null, options: CallOptions): Promise<Handle>;
  next(handle: Handle, mode: 'read' | 'tail', options: CallOptions): Promise<StreamItem | null>;
  cancel(handle: Handle, mode: 'read' | 'tail'): void;
  connectContext(options: ContextOptions, call: CallOptions): Promise<Handle>;
  ensureContext(handle: Handle, chronicle: string, name: string, call: CallOptions): Promise<ContextRef>;
  listContexts(handle: Handle, chronicle: string, call: CallOptions): Promise<ContextRef[]>;
  open(handle: Handle, context: ContextRef, identity: AgentIdentity, options: OpenOptions, call: CallOptions): Promise<{ handle: Handle; context: ContextRef; identity: AgentIdentity }>;
  remember(handle: Handle, memory: Memory, options: RememberOptions, call: CallOptions): Promise<MemoryResult>;
  recall(handle: Handle, options: RecallOptions, call: CallOptions): Promise<Page>;
  latest(handle: Handle, n: number, options: LatestOptions, call: CallOptions): Promise<LatestResult>;
  reconcile(handle: Handle, options: ReconcileOptions, call: CallOptions): Promise<ReconcileResult>;
  acknowledgeProcessed(handle: Handle, position: Position): void;
  checkpoint(handle: Handle): Checkpoint;
  sessionStatus(handle: Handle): SessionStatus;
  close(handle: Handle, call: CallOptions): Promise<CloseResult>;
  follow(handle: Handle, inputs: { session: Handle; from?: FollowFrom; after?: Position }[], options: FollowOptions, call: CallOptions): Promise<FollowResult>;
  encodeCheckpoint(checkpoint: Checkpoint, maxBytes?: number): Uint8Array;
  decodeCheckpoint(bytes: Uint8Array): Checkpoint;
}
const core: Core = require(join(__dirname, '../build/dev/chronolog_node.node'));
function freeze<T>(value: T): T {
  if (value && typeof value === 'object' && !ArrayBuffer.isView(value)) {
    for (const child of Object.values(value)) freeze(child);
    Object.freeze(value);
  }
  return value;
}
export async function connect(options: ConnectOptions): Promise<Client> {
  return new Client(await invoke(() => core.connect(options)));
}
export class Client {
  constructor(private readonly handle: Handle) {}
  private async catalog<T>(method: string, args: unknown[], options: CallOptions): Promise<T> {
    return freeze(await invoke(() => core.catalog(this.handle, method, args, options), options.signal) as T);
  }
  createChronicle(name: string, options: CallOptions = {}): Promise<Chronicle> { return this.catalog('createChronicle', [name], options); }
  getChronicle(name: string, options: CallOptions = {}): Promise<Chronicle> { return this.catalog('getChronicle', [name], options); }
  listChronicles(options: CallOptions = {}): Promise<readonly Chronicle[]> { return this.catalog('listChronicles', [], options); }
  async destroyChronicle(name: string, options: CallOptions = {}): Promise<void> { await this.catalog('destroyChronicle', [name], options); }
  createStory(chronicle: string, name: string, options: CallOptions = {}): Promise<Story> { return this.catalog('createStory', [chronicle, name], options); }
  getStory(story: bigint, options: CallOptions = {}): Promise<Story> { return this.catalog('getStory', [story], options); }
  listStories(chronicle: string, options: CallOptions = {}): Promise<readonly Story[]> { return this.catalog('listStories', [chronicle], options); }
  async destroyStory(story: bigint, options: CallOptions = {}): Promise<void> { await this.catalog('destroyStory', [story], options); }
  async acquire(story: bigint, identity: string, options: AcquireOptions = {}): Promise<Writer> {
    const result = await invoke(() => core.acquire(this.handle, story, identity, options), options.signal);
    return new Writer(result.handle, freeze(result.acquisition));
  }
  // A random 128-bit id owned by this Client in this process, to retain one logical acquire across calls.
  newAcquireRequestId(): string { return sync(() => core.newAcquireRequestId(this.handle)); }
  readPhysical(story: bigint, range: PhysicalRange, options: StreamOptions = {}): EventStream {
    return new EventStream(() => invoke(() => core.stream(this.handle, story, 'physical', range, options)), 'read', options);
  }
  read(story: bigint, range: HlcRange, options: StreamOptions = {}): EventStream {
    return new EventStream(() => invoke(() => core.stream(this.handle, story, 'read', range, options)), 'read', options);
  }
  tail(story: bigint, after: Position | null = null, options: StreamOptions = {}): EventStream {
    return new EventStream(() => invoke(() => core.stream(this.handle, story, 'tail', after, options)), 'tail', options);
  }
}
export class Writer {
  constructor(private readonly handle: Handle, readonly acquisition: Acquisition) {}
  async append(payload: Uint8Array, options: AppendOptions = {}): Promise<AppendResult> {
    return freeze(await invoke(() => core.append(this.handle, payload, options), options.signal));
  }
  async appendBatch(items: readonly AppendItem[], options: CallOptions = {}): Promise<(AppendResult | ChronologError)[]> {
    const values = await invoke(() => core.appendBatch(this.handle, items, options), options.signal);
    return values.map(value => value instanceof Error ? translate(value) as ChronologError : freeze(value));
  }
  // Stops renewal before sending Release.
  release(options: CallOptions = {}): Promise<boolean> { return invoke(() => core.release(this.handle, options), options.signal); }
  lease(): WriterLease { return freeze(sync(() => core.lease(this.handle))); }
}
export class EventStream implements AsyncIterableIterator<Event> {
  readonly completion: Promise<Completion>;
  continuation?: Hlc;
  private resolveCompletion!: (value: Completion) => void;
  private rejectCompletion!: (error: Error) => void;
  private handle?: Handle;
  private readonly opened: Promise<Handle>;
  private events: Event[] = [];
  private index = 0;
  private closed = false;
  private completed = false;
  private pulling = false;
  private failure?: Error;
  private readonly abort = () => this.cancel();
  constructor(open: () => Promise<Handle>, private readonly mode: 'read' | 'tail', private readonly options: StreamOptions) {
    this.completion = new Promise((resolve, reject) => { this.resolveCompletion = resolve; this.rejectCompletion = reject; });
    void this.completion.catch(() => {});
    this.opened = Promise.resolve().then(open).then(handle => {
      this.handle = handle;
      if (this.closed) core.cancel(handle, this.mode);
      return handle;
    }).catch(error => {
      this.fail(translate(error));
      throw this.failure;
    });
    void this.opened.catch(() => {});
    if (options.signal?.aborted) this.cancel();
    else options.signal?.addEventListener('abort', this.abort, { once: true });
  }
  [Symbol.asyncIterator](): AsyncIterableIterator<Event> { return this; }
  private detach(): void { this.options.signal?.removeEventListener('abort', this.abort); }
  private fail(error: Error): void {
    this.failure ??= error;
    this.closed = true;
    this.detach();
    if (this.handle) core.cancel(this.handle, this.mode);
    if (!this.completed) this.rejectCompletion(this.failure);
  }
  cancel(): void { this.fail(new Cancelled('CANCELLED', 'stream cancelled', { code: 1, message: 'stream cancelled' })); }
  async next(): Promise<IteratorResult<Event>> {
    if (this.pulling) throw new FailedPrecondition('FAILED_PRECONDITION', 'one consumer per stream', { code: 9, message: 'one consumer per stream' });
    this.pulling = true;
    try {
      if (this.failure) throw this.failure;
      if (this.closed) return { done: true, value: undefined };
      const handle = await this.opened;
      if (this.failure) throw this.failure;
      while (this.index >= this.events.length) {
        const item = await invoke(() => core.next(handle, this.mode, this.options));
        if (this.failure) throw this.failure;
        if (item === null) {
          this.closed = true;
          this.detach();
          if (!this.completed) throw new DataLoss('DATA_LOSS', 'stream ended without Completion', { code: 15, message: 'stream ended without Completion' });
          return { done: true, value: undefined };
        }
        this.events = item.events.map(event => freeze(event));
        this.index = 0;
        if (item.completion) {
          this.completed = true;
          this.continuation = item.continuation && freeze(item.continuation);
          this.resolveCompletion(freeze(item.completion));
          this.detach();
          if (!this.events.length) {
            this.closed = true;
            return { done: true, value: undefined };
          }
        }
      }
      return { done: false, value: this.events[this.index++] };
    } catch (error) { this.fail(translate(error)); throw this.failure; }
    finally { this.pulling = false; }
  }
  async return(): Promise<IteratorResult<Event>> {
    if (!this.closed) this.cancel();
    return { done: true, value: undefined };
  }
  async throw(error: unknown): Promise<IteratorResult<Event>> { this.fail(translate(error)); throw this.failure; }
}

export interface ContextRef { readonly storyId: bigint; readonly chronicle: string; readonly name: string }
export interface AgentIdentity { readonly agentId: string; readonly slot: string }
export interface WriterStamp { readonly writerId: bigint; readonly incarnation: bigint }
export type Access = 'READ_ONLY' | 'READ_WRITE' | UnknownEnum;
export type SessionState = 'READY' | 'TRANSPORT_PENDING' | 'NEEDS_RECONCILE' | 'FENCED' | 'CLOSED' | UnknownEnum;
export type MemoryOutcome = 'DURABLE' | 'RAM_ONLY_MAY_VANISH' | 'REJECTED' | 'UNKNOWN' | 'FENCED' | 'LANDED' | UnknownEnum;
export type DeliveryLimit = 'NONE' | 'EVENTS' | 'BYTES' | 'OVERSIZED_EVENT' | 'READ_CALLS' | UnknownEnum;
export type ReconcileOutcome = 'LANDED' | 'ABSENT' | 'UNKNOWN' | UnknownEnum;
export type FollowFrom = 'NOW' | 'BEGINNING' | 'POSITION' | UnknownEnum;
export interface PageLimits { maxEvents?: number; maxRawBytes?: number }
export interface EnvelopeInput {
  payload: Uint8Array; contentType?: string; attributes?: Record<string, string>; traceId?: Uint8Array; spanId?: Uint8Array;
}
export interface Memory { operationId: string; envelope: EnvelopeInput; durability?: Durability; physical?: TimeReading }
export interface PriorOutcome {
  readonly operationId: string; readonly status: Status; readonly outcome: MemoryOutcome; readonly receipt?: AppendResult;
  readonly landed?: Position; readonly observedDurability: Durability | number;
}
export interface MemoryResult {
  readonly current: PriorOutcome; readonly resolvedPrior: readonly PriorOutcome[]; readonly blockingOperationId?: string;
  readonly state: SessionState;
}
export interface RememberOptions { resendAfterAbsent?: boolean }
export interface Page {
  readonly events: readonly Event[]; readonly range?: HlcRange; readonly completion?: Completion; readonly completionRange?: HlcRange;
  readonly streamStatus: Status; readonly limited: DeliveryLimit; readonly rawBytes: number; readonly answerComplete: boolean;
  readonly hasMore: boolean; readonly idle: boolean; readonly cutCoversCausalFloor: boolean; readonly after?: Position;
  readonly nextCursor?: string; readonly deliveredPrefixEnd?: Hlc;
}
export interface RecallOptions { start?: Hlc; end?: Hlc; cursor?: string; limits?: PageLimits; maxReadCalls?: number }
export interface LatestOptions { before?: Hlc; limits?: PageLimits; maxReadCalls?: number }
export interface LatestResult { readonly page: Page; readonly asOf?: Hlc; readonly selectionComplete: boolean }
export interface AcquisitionProvenance {
  readonly hostId: string; readonly launcherLockId: string; readonly expectedPriorIncarnation?: bigint;
  readonly acquisitionRecordReceiptHlc?: Hlc;
}
export interface RecoveryIncarnation { readonly writer: WriterStamp; readonly markerOperationId?: string }
export interface ReconcileCheckpoint {
  readonly transitionId: string; readonly lowerBound: Hlc; readonly recoveredIncarnations: readonly RecoveryIncarnation[];
  readonly currentMarkerOperationId?: string; readonly markerHlc?: Hlc;
}
export interface ReconciledOperation {
  readonly operationId: string; readonly outcome: ReconcileOutcome; readonly landed?: Position; readonly observedDurability: Durability | number;
}
export interface OperationDisposition {
  readonly result: ReconciledOperation; readonly priorWriter: WriterStamp; readonly normalizedDigest?: Uint8Array;
}
export interface UnknownOperation {
  readonly operationId: string; readonly incarnations: readonly WriterStamp[]; readonly window?: HlcRange;
  readonly absenceProvable: boolean; readonly normalizedDigest?: Uint8Array;
}
// A value snapshot of a session; it never carries an acquire request id.
export interface Checkpoint {
  readonly identity: AgentIdentity; readonly context: ContextRef; readonly causalFloor: Hlc; readonly processedAfter?: Position;
  readonly writer?: WriterStamp; readonly acquisition?: AcquisitionProvenance; readonly lastOwnReceiptHlc?: Hlc;
  readonly recovery?: ReconcileCheckpoint; readonly unresolvedOperations: readonly string[];
  readonly permanentlyUnknownOperations: readonly UnknownOperation[]; readonly dispositions: readonly OperationDisposition[];
  readonly priorStateUnknownBelow?: Hlc; readonly acquisitionClosed: boolean; readonly reconcileAttempted: boolean;
  readonly takeoverRequired: boolean;
}
export interface OpenOptions { access?: Access; sessionId?: string; resume?: Checkpoint; ownership?: AcquisitionProvenance }
export interface ReconcileOptions { operationIds?: readonly string[]; takeover?: boolean; maxReadCalls?: number }
export interface ReconcileResult {
  readonly writer?: WriterStamp; readonly markerHlc?: Hlc; readonly range?: HlcRange; readonly operations: readonly ReconciledOperation[];
  readonly completion?: Completion; readonly status: Status; readonly attempted: boolean;
  readonly permanentlyUnknownOperations: readonly string[]; readonly proofComplete: boolean;
  readonly supplyAllUnseenOperationIds: boolean; readonly omittedOperationIdsMayDuplicate: boolean;
}
export interface CloseResult { readonly releaseCommitted: boolean; readonly fenced: boolean }
export interface SessionStatus {
  readonly context: ContextRef; readonly identity: AgentIdentity; readonly writer?: WriterStamp; readonly state: SessionState;
  readonly blockingOperationId?: string; readonly unresolvedOperations: readonly string[];
  readonly permanentlyUnknownOperations: readonly string[]; readonly reconcileAttempted: boolean; readonly causalFloor: Hlc;
}
export interface ContextOptions extends ConnectOptions {
  maxWritableSessions?: number; maxPendingOperationBytes?: number; maxCompletedOperations?: number;
  maxUnresolvedOperations?: number; maxPersistedDispositions?: number; maxCheckpointPayloadBytes?: number;
  cutProbeWidthNs?: bigint;
}
export interface FollowInput { session: ContextSession; from?: FollowFrom; after?: Position }
// waitMs bounds an idle poll; the native wait has whole-second granularity, so a partial second rounds up.
export interface FollowOptions { limits?: PageLimits; waitMs?: number }
export interface FollowPage {
  readonly context: ContextRef; readonly page: Page; readonly resume?: Position; readonly startingCut?: Hlc;
  readonly uncertifiedRouteKeepers: readonly string[];
}
export interface FollowResult { readonly pages: readonly FollowPage[]; readonly status: Status; readonly idle: boolean }

const sessions = new WeakMap<ContextSession, Handle>();
function sessionHandle(session: ContextSession): Handle {
  const handle = sessions.get(session);
  if (!handle) throw new TypeError('not a ContextSession');
  return handle;
}
export async function connectContext(options: ContextOptions, call: CallOptions = {}): Promise<ContextClient> {
  const timeoutMs = call.timeoutMs ?? options.timeoutMs;
  return new ContextClient(await invoke(() => core.connectContext(options, { timeoutMs }), call.signal));
}
export function encodeCheckpoint(checkpoint: Checkpoint, maxBytes?: number): Uint8Array {
  return sync(() => core.encodeCheckpoint(checkpoint, maxBytes));
}
export function decodeCheckpoint(bytes: Uint8Array): Checkpoint { return freeze(sync(() => core.decodeCheckpoint(bytes))); }
export class ContextClient {
  constructor(private readonly handle: Handle) {}
  async ensureContext(chronicle: string, name: string, call: CallOptions = {}): Promise<ContextRef> {
    return freeze(await invoke(() => core.ensureContext(this.handle, chronicle, name, call), call.signal));
  }
  async listContexts(chronicle: string, call: CallOptions = {}): Promise<readonly ContextRef[]> {
    return freeze(await invoke(() => core.listContexts(this.handle, chronicle, call), call.signal));
  }
  async open(context: ContextRef, identity: AgentIdentity, options: OpenOptions = {}, call: CallOptions = {}): Promise<ContextSession> {
    const opened = await invoke(() => core.open(this.handle, context, identity, options, call), call.signal);
    return new ContextSession(opened.handle, freeze(opened.context), freeze(opened.identity));
  }
  // One bounded poll over every input with a shared deadline.
  async follow(inputs: readonly FollowInput[], options: FollowOptions = {}, call: CallOptions = {}): Promise<FollowResult> {
    const native = inputs.map(input => ({ session: sessionHandle(input.session), from: input.from, after: input.after }));
    return freeze(await invoke(() => core.follow(this.handle, native, options, call), call.signal));
  }
  // Polls follow until aborted, yielding every result that is not idle and resuming each input after its last
  // delivered Position. A failed poll throws its typed status with the partial result attached as `result`.
  async *subscribe(inputs: readonly FollowInput[], options: FollowOptions = {}, call: CallOptions = {}): AsyncGenerator<FollowResult, void, undefined> {
    let current = [...inputs];
    for (;;) {
      if (call.signal?.aborted) throw cancelled();
      const result = await this.follow(current, options, call);
      if (result.status.code !== 'OK') {
        const status = result.status;
        const failure = translate({ ...status, itemStatus: { code: codes.indexOf(status.code), message: status.message } });
        Object.defineProperty(failure, 'result', { value: result, enumerable: true });
        throw failure;
      }
      current = current.map((input, i) => {
        const resume = result.pages[i]?.resume;
        return resume ? { session: input.session, from: 'POSITION' as const, after: resume } : input;
      });
      if (!result.idle) yield result;
    }
  }
}
const codes: StatusCode[] = ['OK', 'CANCELLED', 'UNKNOWN', 'INVALID_ARGUMENT', 'DEADLINE_EXCEEDED', 'NOT_FOUND', 'ALREADY_EXISTS',
  'PERMISSION_DENIED', 'RESOURCE_EXHAUSTED', 'FAILED_PRECONDITION', 'ABORTED', 'OUT_OF_RANGE', 'UNIMPLEMENTED', 'INTERNAL',
  'UNAVAILABLE', 'DATA_LOSS', 'UNAUTHENTICATED'];
export class ContextSession {
  constructor(handle: Handle, readonly context: ContextRef, readonly identity: AgentIdentity) { sessions.set(this, handle); }
  private get handle(): Handle { return sessionHandle(this); }
  async remember(memory: Memory, options: RememberOptions = {}, call: CallOptions = {}): Promise<MemoryResult> {
    return freeze(await invoke(() => core.remember(this.handle, memory, options, call), call.signal));
  }
  async recall(options: RecallOptions = {}, call: CallOptions = {}): Promise<Page> {
    return freeze(await invoke(() => core.recall(this.handle, options, call), call.signal));
  }
  async latest(n: number, options: LatestOptions = {}, call: CallOptions = {}): Promise<LatestResult> {
    return freeze(await invoke(() => core.latest(this.handle, n, options, call), call.signal));
  }
  async reconcile(options: ReconcileOptions = {}, call: CallOptions = {}): Promise<ReconcileResult> {
    return freeze(await invoke(() => core.reconcile(this.handle, options, call), call.signal));
  }
  acknowledgeProcessed(position: Position): void { sync(() => core.acknowledgeProcessed(this.handle, position)); }
  checkpoint(): Checkpoint { return freeze(sync(() => core.checkpoint(this.handle))); }
  status(): SessionStatus { return freeze(sync(() => core.sessionStatus(this.handle))); }
  async close(call: CallOptions = {}): Promise<CloseResult> {
    return freeze(await invoke(() => core.close(this.handle, call), call.signal));
  }
}
