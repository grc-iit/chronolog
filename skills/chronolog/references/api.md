# ChronoLog client API

All three SDKs share one implementation: the C++ client in `client/cpp`. Python and TypeScript are thin bindings over it, so semantics (retries, deduplication, causal floors, Completion) are identical.

## Python (`chronolog`, wheel `chronolog-4.0.0-cp312-abi3-linux_x86_64.whl`)

```python
connect(catalog, player=None, *, timeout=10.0, max_retries=3, retry_backoff=0.02,
        max_in_flight=4, batch_size=128, max_batch_items=10000, max_batch_bytes=64 << 20,
        channel_args=None) -> Client
```

Client (every method also takes keyword `timeout=`; usable as a context manager):

| Method | Notes |
|---|---|
| `create_chronicle(name)`, `chronicle(name)`, `list_chronicles()`, `destroy_chronicle(c)` | `create_*` raises `AlreadyExists` when present |
| `create_story(chronicle, name)`, `story(id)`, `list_stories(chronicle)`, `destroy_story(s)` | no find-by-name: filter `list_stories` on `name` and `not tombstoned` |
| `acquire(story, identity) -> Writer` | one identity per logical writer; a new acquisition gets a new incarnation |
| `read(story, start=None, end=None) -> ReadStream` | half-open HLC range; defaults cover everything |
| `tail(story, after=None) -> TailStream` | `after` is an Event (anything with `.hlc` and `.id`); strictly after it |
| `read_physical(story, start_ns, end_ns) -> ReadStream` | wall-clock range in ns; the SDK splits ranges that hit the read limit |

Writer: `append(payload, *, content_type=None, attributes=None, trace_id=None, span_id=None, durability=Durability.DURABLE, timeout=None) -> AppendResult`; `append_batch(items, *, durability=..., timeout=None)` where items are bytes, `Envelope`, `AppendSpec` or dicts, and a failed item comes back as an `Error` instance in the list; `release()`. Properties: `writer_id`, `incarnation`, `story_id`, `route`. Exiting a `with` block releases.

Streams are iterators of `Event`; after the last event, `.completion` (Completion) and `.continuation` are set. `cancel()` stops them; exiting a `with` block cancels.

Types (frozen dataclasses unless noted): `Hlc(physical_ns, logical)` ordered; `EventId(story_id, writer_id, incarnation, sequence)`; `Envelope(payload, content_type, attributes, trace_id, span_id)`; `Event(id, physical, hlc, envelope, durability)` with `.payload`; `AppendResult(event_id, hlc, durability)` with `.acked` (DURABLE only); `Completion(complete, frontier, laggards, reason)`; `Chronicle(name, tombstoned)`; `Story(id, chronicle, name, epoch, tombstoned)`; `TimeReading(physical_ns, uncertainty_ns, status)` (status 0 Synced, 1 Unsynced, 2 Unavailable). Enums: `Durability` (UNSPECIFIED, ACCEPTED, DURABLE), `IncompleteReason` (NONE, LAGGING_WRITERS, PHYSICAL_AXIS_UNBOUNDED, SOURCE_FAILED, TRUNCATED).

Errors: `Error` with `.status`, and subclasses `Unavailable`, `FailedPrecondition`, `InvalidArgument`, `OutOfRange`, `Unimplemented`, `NotFound`, `Cancelled`, `DeadlineExceeded`, `AlreadyExists`, `ResourceExhausted`.

Read until complete (fresh writes become complete once the Keeper seals past them):

```python
import time

def read_complete(client, story, start, end, deadline_s=5.0):
    stop = time.monotonic() + deadline_s
    while True:
        with client.read(story, start, end) as reader:
            events = list(reader)
            if reader.completion.complete or time.monotonic() > stop:
                return events, reader.completion
        time.sleep(0.1)
```

Follow a story live:

```python
with client.tail(story, after=last_event) as stream:
    for event in stream:
        handle(event)
```

OpenTelemetry GenAI spans (`pip install 'chronolog[otel]'`): one story per `gen_ai.conversation.id`, content type `application/vnd.chronolog.otel-span+json`, trace and span ids carried in the envelope.

```python
from opentelemetry.sdk.trace import TracerProvider
from opentelemetry.sdk.trace.export import SimpleSpanProcessor
from chronolog.otel import ChronologSpanExporter

provider = TracerProvider()
provider.add_span_processor(SimpleSpanProcessor(ChronologSpanExporter(
    "127.0.0.1:50051", "127.0.0.1:50054", chronicle="otel", default_story="spans")))
```

## C++ (`chronolog::client`, header `chronolog/client/client.h`, CMake package `chronolog`)

```cpp
auto client = chronolog::client::Client::Connect({.catalog_endpoint = "127.0.0.1:50051",
                                                  .player_endpoint = "127.0.0.1:50054"});
auto story = client->createStory("my-chronicle", "notes", deadline);   // after createChronicle
auto writer = client->acquire(story->id, "planner-1", deadline);
auto result = writer->append({.envelope = {.content_type = "text/plain", .payload = "hello"}}, deadline);
auto stream = client->read(story->id, {start, end}, deadline);
for(;;)
{
    auto item = stream->next(deadline);                  // StatusOr<std::optional<StreamItem>>
    if(!item.ok() || !item->has_value())
        break;
    for(const auto& event: (*item)->events) { /* event.id, event.hlc, event.envelope */ }
    if((*item)->completion) { /* complete, frontier, laggards, reason */ }
}
```

Everything returns `absl::StatusOr`. `ClientOptions` holds `catalog_endpoint`, `player_endpoint`, `rpc_timeout` (10 s), `retry` (3 retries, 20 ms backoff), `max_in_flight`, `batch_size` and batch limits. `Writer::appendBatch(std::span<const AppendSpec>)` returns per-item results. `tail(story, std::optional<Position> after)` follows live. `readPhysical(story, {start_ns, end_ns})` reads by wall-clock time. The shortest complete program is `client/cpp/tests/acceptance.cpp`.

## TypeScript (`@chronolog/client`, Node 22+)

```js
import { connect } from '@chronolog/client';
const client = await connect({ catalog: '127.0.0.1:50051', player: '127.0.0.1:50054' });
await client.createChronicle('web');
const story = await client.createStory('web', 'events');
const writer = await client.acquire(story.id, 'ui-1');
await writer.appendBatch([{ payload: Buffer.from('clicked'), contentType: 'text/plain' }]);
const stream = client.read(story.id, { start, end }, { timeoutMs: 5000 });
for await (const event of stream) console.log(event);
console.log(await stream.completion);
```

Story ids are `bigint`. `tail(story, after)` and `readPhysical(story, {startNs, endNs})` mirror the other SDKs; errors are `ChronologError` subclasses.

## MCP server (`chronolog-mcp`, wheel `chronolog_mcp-4.0.0-py3-none-any.whl`)

`chronolog-mcp [--catalog 127.0.0.1:50051] [--player HOST:PORT] [--chronicle chronolog] [--identity NAME] [--timeout 10] [--transport stdio|http] [--host 127.0.0.1] [--port 8000]`. Environment equivalents: `CHRONOLOG_CATALOG`, `CHRONOLOG_PLAYER`, `CHRONOLOG_CHRONICLE`, `CHRONOLOG_WRITER_IDENTITY`, `MCP_TRANSPORT`.

| Tool | Parameters |
|---|---|
| `list_stories` | `chronicle=None` |
| `create_story` | `story`, `chronicle=None` |
| `append` | `story` (id), `content`, `content_type="text/plain"`, `attributes`, `trace_id`, `span_id` |
| `read` | `story`, `start_hlc`, `end_hlc`, `limit=1000`; returns events, `completion`, `limited`, `after` |
| `tail` | `story`, `after`, `max_events=100`, `timeout_s=5.0` |
| `start_chronolog`, `record_interaction`, `retrieve_interaction`, `stop_chronolog` | conversation logging workflow |

Resource `chronolog://status`; prompt `logging_workflow`. `chronolog-read STORY [--start-hlc JSON] [--end-hlc JSON] [--limit N]` prints one read as JSON.

## Command-line tools

| Program | Usage |
|---|---|
| `chronolog_kvs` | `chronolog_kvs CATALOG PLAYER CHRONICLE put\|get\|get-at\|erase\|history KEY [VALUE\|HLC\|START END]`; an HLC is `physical_ns:logical` |
| `chronolog_sql` | `chronolog_sql CATALOG PLAYER CHRONICLE [STATEMENT ...]`, or statements on stdin. Grammar: `CREATE TABLE`, `INSERT INTO ... VALUES`, `SELECT cols\|*\|COUNT(*) FROM t [WHERE ...] [ORDER BY TIME [ASC\|DESC]] [LIMIT n]` with `TIME BETWEEN` and `PHYSICAL BETWEEN` ranges; anything else is rejected naming the token |
| `chronolog_admin` | `chronolog_admin VISOR_INTERNAL_ENDPOINT list\|drain\|join\|abandon [PROCESS_ID]` (dynamic membership) |
| `chronolog_stream_collect` | `--visor --player --chronicle host-metrics --host NAME --interval-ms 1000 --samples 0 --durability durable\|accepted` |
| `chronolog_stream_export` | `--visor --player --chronicle host-metrics --influx-url --org chronolog --bucket telemetry --token --batch-count 128 --batch-age-ms 250` |
| `chronolog_pubsub_example`, `chronolog_kvs_example`, `chronolog_sql_example` | `CATALOG PLAYER`; copyable starting points |
