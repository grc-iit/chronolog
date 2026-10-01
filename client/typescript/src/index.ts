import { join } from 'node:path';

export enum Durability { UNSPECIFIED = 0, ACCEPTED = 1, DURABLE = 2 }
export type StatusCode = 'OK' | 'CANCELLED' | 'UNKNOWN' | 'INVALID_ARGUMENT' | 'DEADLINE_EXCEEDED' |
  'NOT_FOUND' | 'ALREADY_EXISTS' | 'PERMISSION_DENIED' | 'RESOURCE_EXHAUSTED' | 'FAILED_PRECONDITION' |
  'ABORTED' | 'OUT_OF_RANGE' | 'UNIMPLEMENTED' | 'INTERNAL' | 'UNAVAILABLE' | 'DATA_LOSS' | 'UNAUTHENTICATED';
export interface ItemStatus { readonly code: number; readonly message: string }
export class ChronologError extends Error {
  readonly itemStatus: ItemStatus;
  constructor(readonly code: StatusCode, message: string, itemStatus: ItemStatus) {
    super(message);
    this.name = new.target.name;
    this.itemStatus = Object.freeze({ ...itemStatus });
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
    const raw = error as { code: StatusCode; message: string; itemStatus: ItemStatus };
    return new (errors[raw.code] ?? ChronologError)(raw.code, raw.message, raw.itemStatus);
  }
  return error instanceof Error ? error : new Error(String(error));
}
async function call<T>(operation: () => Promise<T>): Promise<T> {
  try { return await operation(); } catch (error) { throw translate(error); }
}
export interface Hlc { readonly physicalNs: bigint; readonly logical: number }
export interface EventId { readonly storyId: bigint; readonly writerId: bigint; readonly incarnation: bigint; readonly sequence: bigint }
export interface KeeperRef { readonly processId: string; readonly endpoint: string }
export interface Route { readonly epoch: bigint; readonly keepers: readonly KeeperRef[]; readonly grapher: string; readonly player: string }
export interface Acquisition { readonly storyId: bigint; readonly writerId: bigint; readonly incarnation: bigint; readonly route: Route; readonly assignedKeeper: KeeperRef }
export interface Envelope { readonly contentType: string; readonly payload: Uint8Array; readonly traceId: Uint8Array; readonly spanId: Uint8Array; readonly attributes: Readonly<Record<string, string>> }
export interface TimeReading { readonly physicalNs: bigint; readonly uncertaintyNs?: bigint; readonly status: 'SYNCED' | 'UNSYNCED' | 'UNAVAILABLE' }
export interface Event { readonly id: EventId; readonly hlc: Hlc; readonly physical: TimeReading; readonly envelope: Envelope; readonly durability: Durability }
export interface Frontier { readonly writerId: bigint; readonly incarnation: bigint; readonly frontier: Hlc }
export interface Completion { readonly complete: boolean; readonly frontier: Hlc; readonly laggards: readonly Frontier[]; readonly reason: 'NONE' | 'LAGGING_WRITERS' | 'PHYSICAL_AXIS_UNBOUNDED' | 'SOURCE_FAILED' | 'TRUNCATED' }
export interface Chronicle { readonly name: string; readonly tombstoned: boolean }
export interface Story { readonly id: bigint; readonly epoch: bigint; readonly chronicle: string; readonly name: string; readonly tombstoned: boolean }
export interface AppendResult { readonly eventId: EventId; readonly hlc: Hlc; readonly achieved: Durability; readonly acked: boolean }
export interface HlcRange { readonly start: Hlc; readonly end: Hlc }
export interface Position { readonly hlc: Hlc; readonly id: EventId }
export interface CallOptions { timeoutMs?: number }
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
export interface StreamOptions extends CallOptions { signal?: AbortSignal }
interface StreamItem { events: Event[]; completion?: Completion; continuation?: Hlc }
type Handle = object;
interface Core {
  connect(options: ConnectOptions): Promise<Handle>;
  catalog(handle: Handle, method: string, args: unknown[], options: CallOptions): Promise<unknown>;
  acquire(handle: Handle, story: bigint, identity: string, options: CallOptions): Promise<{ handle: Handle; acquisition: Acquisition }>;
  append(handle: Handle, payload: Uint8Array, options: AppendOptions): Promise<AppendResult>;
  appendBatch(handle: Handle, items: readonly AppendItem[], options: CallOptions): Promise<(AppendResult | Error)[]>;
  release(handle: Handle, options: CallOptions): Promise<boolean>;
  stream(handle: Handle, story: bigint, mode: 'read' | 'tail', input: HlcRange | Position | null, options: CallOptions): Promise<Handle>;
  next(handle: Handle, mode: 'read' | 'tail', options: CallOptions): Promise<StreamItem | null>;
  cancel(handle: Handle, mode: 'read' | 'tail'): void;
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
  return new Client(await call(() => core.connect(options)));
}
export class Client {
  constructor(private readonly handle: Handle) {}
  private async catalog<T>(method: string, args: unknown[], options: CallOptions): Promise<T> {
    return freeze(await call(() => core.catalog(this.handle, method, args, options)) as T);
  }
  createChronicle(name: string, options: CallOptions = {}): Promise<Chronicle> { return this.catalog('createChronicle', [name], options); }
  getChronicle(name: string, options: CallOptions = {}): Promise<Chronicle> { return this.catalog('getChronicle', [name], options); }
  listChronicles(options: CallOptions = {}): Promise<readonly Chronicle[]> { return this.catalog('listChronicles', [], options); }
  async destroyChronicle(name: string, options: CallOptions = {}): Promise<void> { await this.catalog('destroyChronicle', [name], options); }
  createStory(chronicle: string, name: string, options: CallOptions = {}): Promise<Story> { return this.catalog('createStory', [chronicle, name], options); }
  getStory(story: bigint, options: CallOptions = {}): Promise<Story> { return this.catalog('getStory', [story], options); }
  listStories(chronicle: string, options: CallOptions = {}): Promise<readonly Story[]> { return this.catalog('listStories', [chronicle], options); }
  async destroyStory(story: bigint, options: CallOptions = {}): Promise<void> { await this.catalog('destroyStory', [story], options); }
  async acquire(story: bigint, identity: string, options: CallOptions = {}): Promise<Writer> {
    const result = await call(() => core.acquire(this.handle, story, identity, options));
    return new Writer(result.handle, freeze(result.acquisition));
  }
  read(story: bigint, range: HlcRange, options: StreamOptions = {}): EventStream {
    return new EventStream(() => call(() => core.stream(this.handle, story, 'read', range, options)), 'read', options);
  }
  tail(story: bigint, after: Position | null = null, options: StreamOptions = {}): EventStream {
    return new EventStream(() => call(() => core.stream(this.handle, story, 'tail', after, options)), 'tail', options);
  }
}
export class Writer {
  constructor(private readonly handle: Handle, readonly acquisition: Acquisition) {}
  async append(payload: Uint8Array, options: AppendOptions = {}): Promise<AppendResult> {
    return freeze(await call(() => core.append(this.handle, payload, options)));
  }
  async appendBatch(items: readonly AppendItem[], options: CallOptions = {}): Promise<(AppendResult | ChronologError)[]> {
    const values = await call(() => core.appendBatch(this.handle, items, options));
    return values.map(value => value instanceof Error ? translate(value) as ChronologError : freeze(value));
  }
  release(options: CallOptions = {}): Promise<boolean> { return call(() => core.release(this.handle, options)); }
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
        const item = await call(() => core.next(handle, this.mode, this.options));
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
