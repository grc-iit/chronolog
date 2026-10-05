const { test } = require('node:test');
const assert = require('node:assert/strict');
const { setTimeout: delay } = require('node:timers/promises');
const {
  connect, Durability, ChronologError, NotFound, FailedPrecondition, Cancelled, DeadlineExceeded,
} = require('../dist');

const catalog = process.env.CHRONOLOG_TYPESCRIPT_CATALOG || '127.0.0.1:50051';
const player = process.env.CHRONOLOG_TYPESCRIPT_PLAYER || '127.0.0.1:50054';
const options = { catalog, player, rpcTimeoutMs: 8000, batchSize: 64, maxInFlight: 4 };
const payload = sequence => Buffer.from(`ts-event-${sequence}`);

async function create(client, name) {
  const chronicle = `typescript-${name}-${process.pid}-${Date.now()}`;
  await client.createChronicle(chronicle);
  return { chronicle, story: await client.createStory(chronicle, 'events') };
}
async function complete(client, story, results) {
  const range = { start: results[0].hlc,
    end: { physicalNs: results.at(-1).hlc.physicalNs, logical: results.at(-1).hlc.logical + 1 } };
  let last;
  for (let attempt = 0; attempt < 40; ++attempt) {
    const stream = client.read(story, range, { timeoutMs: 5000 });
    const events = [];
    try {
      for await (const event of stream) {
        events.push(event);
        assert.ok(events.length <= results.length);
      }
      const completion = await stream.completion;
      last = { completion, count: events.length };
      if (completion.complete) return { events, completion, stream };
    } catch (error) {
      last = { code: error.code, message: error.message };
      if (!['FAILED_PRECONDITION', 'UNAVAILABLE'].includes(error.code)) throw error;
    }
    await delay(50);
  }
  throw new Error(`read did not become complete: ${JSON.stringify(last, (_, value) => typeof value === 'bigint' ? value.toString() : value)}`);
}

test('catalog, 1000 ordered DURABLE appends, exact binary replay, exclusive tail, fenced release', { timeout: 55000 }, async () => {
  const client = await connect(options);
  const { chronicle, story } = await create(client, 'roundtrip');
  assert.equal(typeof story.id, 'bigint');
  assert.deepEqual(await client.getChronicle(chronicle), { name: chronicle, tombstoned: false });
  assert.ok((await client.listChronicles()).some(c => c.name === chronicle));
  assert.deepEqual(await client.getStory(story.id), story);
  assert.deepEqual(await client.listStories(chronicle), [story]);
  const writer = await client.acquire(story.id, 'roundtrip-writer');
  assert.equal(writer.acquisition.incarnation, 1n);
  const traceId = Uint8Array.from({ length: 16 }, (_, i) => i);
  const spanId = Uint8Array.from({ length: 8 }, (_, i) => 255 - i);
  const results = await writer.appendBatch(Array.from({ length: 1000 }, (_, i) => ({
    payload: payload(i + 1), contentType: 'application/octet-stream', attributes: Object.fromEntries([['language', 'typescript'], ['__proto__', 'literal']]), traceId, spanId,
  })));
  assert.equal(results.length, 1000);
  for (const [i, result] of results.entries()) {
    assert.ok(!(result instanceof Error));
    assert.equal(result.eventId.sequence, BigInt(i + 1));
    assert.equal(result.eventId.storyId, story.id);
    assert.equal(typeof result.hlc.physicalNs, 'bigint');
    assert.equal(result.achieved, Durability.DURABLE);
    assert.equal(result.acked, true);
  }
  const { events, completion } = await complete(client, story.id, results);
  assert.equal(completion.reason, 'NONE');
  assert.equal(events.length, 1000);
  for (const [i, event] of events.entries()) {
    assert.deepEqual(event.id, results[i].eventId);
    assert.deepEqual(event.hlc, results[i].hlc);
    assert.equal(typeof event.physical.physicalNs, 'bigint');
    assert.deepEqual(Buffer.from(event.envelope.payload), payload(i + 1));
    assert.deepEqual(event.envelope.attributes, Object.fromEntries([['language', 'typescript'], ['__proto__', 'literal']]));
    assert.deepEqual(event.envelope.traceId, traceId);
    assert.deepEqual(event.envelope.spanId, spanId);
  }
  const physical = client.readPhysical(story.id, { startNs: 0n, endNs: BigInt(Date.now()) * 1000000n + 1000000000n }, { timeoutMs: 8000 });
  const physicalEvents = [];
  for await (const event of physical) physicalEvents.push(event);
  assert.equal(physicalEvents.length, 1000);
  assert.equal((await physical.completion).complete, false);
  const tail = client.tail(story.id, { hlc: events[499].hlc, id: events[499].id });
  let index = 500;
  for await (const event of tail) {
    assert.deepEqual(event.id, events[index].id);
    if (++index === 1000) break;
  }
  assert.equal(index, 1000);
  await assert.rejects(tail.completion, Cancelled);
  assert.equal(await writer.release(), true);
  await assert.rejects(writer.append(payload(1001)), error => error instanceof FailedPrecondition && error.code === 'FAILED_PRECONDITION' && error.itemStatus.code === 9);
  const second = await client.acquire(story.id, 'roundtrip-writer');
  assert.equal(second.acquisition.incarnation, 2n);
  assert.equal(second.acquisition.writerId, writer.acquisition.writerId);
  const accepted = await second.append(Uint8Array.from([0, 255, 0, 128]), { durability: Durability.ACCEPTED, contentType: undefined });
  assert.equal(accepted.eventId.sequence, 1n);
  assert.equal(accepted.acked, false);
  assert.equal(accepted.achieved, Durability.ACCEPTED);
  await second.release();
  await client.destroyStory(story.id);
  await client.destroyChronicle(chronicle);
});

test('kind, actor and links round-trip through append and read', { timeout: 25000 }, async () => {
  const client = await connect(options);
  const { chronicle, story } = await create(client, 'envelope-fields');
  const writer = await client.acquire(story.id, 'envelope-fields-writer');
  try {
    const first = await writer.append(payload(1));
    const dangling = { storyId: story.id, writerId: 99n, incarnation: 1n, sequence: 7n };
    const links = [{ type: 'replies_to', target: first.eventId, targetHlc: first.hlc }, { type: 'cites', target: dangling }];
    const second = await writer.append(payload(2), { kind: 'note.reply', actor: 'agent:ts-test', links });
    const { events } = await complete(client, story.id, [first, second]);
    assert.equal(events.length, 2);
    assert.deepEqual([events[0].envelope.kind, events[0].envelope.actor, events[0].envelope.links], ['', '', []]);
    assert.equal(events[1].envelope.kind, 'note.reply');
    assert.equal(events[1].envelope.actor, 'agent:ts-test');
    assert.deepEqual(events[1].envelope.links, links);
    assert.equal('targetHlc' in events[1].envelope.links[1], false);
  } finally {
    await writer.release();
    await client.destroyStory(story.id);
    await client.destroyChronicle(chronicle);
  }
});

test('awaitEvent finds an appended event and certifies a released incarnation never', { timeout: 25000 }, async () => {
  const client = await connect(options);
  const { chronicle, story } = await create(client, 'await');
  const writer = await client.acquire(story.id, 'await-writer');
  try {
    const first = await writer.append(payload(1));
    const found = await client.awaitEvent(first.eventId, { hlc: first.hlc, boundMs: 10000, timeoutMs: 15000 });
    assert.equal(found.answer, 'FOUND');
    assert.equal('frontier' in found, false);
    assert.deepEqual(found.event.id, first.eventId);
    assert.deepEqual(Buffer.from(found.event.envelope.payload), payload(1));
    assert.equal(await writer.release(), true);
    const later = { ...first.eventId, sequence: 2n };
    const never = await client.awaitEvent(later, { timeoutMs: 5000 });
    assert.deepEqual(never, { answer: 'NEVER' });
  } finally {
    await client.destroyStory(story.id);
    await client.destroyChronicle(chronicle);
  }
});

test('default Read and Tail deliver eight 1 MiB events', { timeout: 25000 }, async () => {
  const client = await connect(options);
  const { chronicle, story } = await create(client, 'receive-limit');
  const writer = await client.acquire(story.id, 'receive-limit-writer');
  try {
    const payloads = Array.from({ length: 8 }, (_, i) => Buffer.alloc(1024 * 1024, i));
    const results = await writer.appendBatch(payloads.map(payload => ({ payload })));
    assert.equal(results.length, 8);
    for (const result of results) assert.ok(!(result instanceof Error) && result.acked);
    const { events, completion } = await complete(client, story.id, results);
    assert.equal(completion.complete, true);
    assert.equal(events.length, 8);
    for (const [i, event] of events.entries()) {
      assert.deepEqual(event.id, results[i].eventId);
      assert.deepEqual(Buffer.from(event.envelope.payload), payloads[i]);
    }
    const tail = client.tail(story.id, null, { timeoutMs: 8000 });
    let delivered = 0;
    try {
      for await (const event of tail) {
        assert.deepEqual(event.id, results[delivered].eventId);
        assert.deepEqual(Buffer.from(event.envelope.payload), payloads[delivered]);
        if (++delivered === 8) break;
      }
      assert.equal(delivered, 8);
    } finally {
      tail.cancel();
      await tail.completion.catch(() => {});
    }
  } finally {
    await writer.release();
    await client.destroyStory(story.id);
    await client.destroyChronicle(chronicle);
  }
});

test('four pending tails do not starve appends or timers on the same event loop', { timeout: 25000 }, async () => {
  const client = await connect(options);
  const { story } = await create(client, 'nonblocking');
  const writer = await client.acquire(story.id, 'tail-writer');
  const seed = await writer.append(Buffer.from('seed'));
  await complete(client, story.id, [seed]);
  const controller = new AbortController();
  const tails = Array.from({ length: 4 }, () => client.tail(story.id, { hlc: seed.hlc, id: seed.eventId }, { signal: controller.signal, timeoutMs: 10000 }));
  const pending = tails.map(tail => tail.next());
  for (const promise of pending) void promise.catch(() => {});
  try {
    await delay(200);
    const before = Date.now();
    const timer = delay(20).then(() => Date.now() - before);
    const appended = await writer.append(Buffer.from('while-tails-wait'), { timeoutMs: 3000 });
    assert.equal(appended.acked, true);
    assert.ok(await timer < 1500, 'event loop timer was blocked');
    for (const result of await Promise.all(pending)) assert.deepEqual(result.value.id, appended.eventId);
    const waiting = tails[0].next();
    const rejected = assert.rejects(waiting, Cancelled);
    controller.abort();
    await rejected;
    for (const tail of tails) await assert.rejects(tail.completion, Cancelled);
  } finally {
    controller.abort();
    await Promise.allSettled(pending);
    await writer.release();
  }
});

test('status taxonomy, bigint range checks and already aborted streams', { timeout: 15000 }, async () => {
  const client = await connect(options);
  await assert.rejects(client.getStory(0xffffffffffffffffn), error => error instanceof NotFound && error instanceof ChronologError && error.itemStatus.code === 5 && error.code === 'NOT_FOUND');
  await assert.rejects(client.getStory(1), TypeError);
  await assert.rejects(client.getStory(-1n), RangeError);
  await assert.rejects(client.getStory(1n << 64n), RangeError);
  const { story } = await create(client, 'validation');
  const controller = new AbortController();
  controller.abort();
  const tail = client.tail(story.id, null, { signal: controller.signal });
  await assert.rejects(tail.next(), Cancelled);
  await assert.rejects(tail.completion, Cancelled);
  tail.cancel();
  const writer = await client.acquire(story.id, 'validation-writer');
  const seed = await writer.append(Buffer.from('seed'));
  await complete(client, story.id, [seed]);
  const timed = client.tail(story.id, { hlc: seed.hlc, id: seed.eventId }, { timeoutMs: 50 });
  await assert.rejects(timed.next(), DeadlineExceeded);
  await assert.rejects(timed.completion, DeadlineExceeded);
  await writer.release();
});

test('appendBatch returns typed item errors in input order', { timeout: 15000 }, async () => {
  const client = await connect(options);
  const { story } = await create(client, 'item-errors');
  const writer = await client.acquire(story.id, 'released-batch-writer');
  await writer.release();
  const results = await writer.appendBatch([{ payload: payload(1) }, { payload: payload(2) }]);
  assert.equal(results.length, 2);
  for (const result of results) {
    assert.ok(result instanceof FailedPrecondition);
    assert.equal(result.code, 'FAILED_PRECONDITION');
    assert.equal(result.itemStatus.code, 9);
  }
});

test('lane writer opens one lane per route Keeper and round-trips appends', { timeout: 30000 }, async () => {
  const client = await connect(options);
  const { chronicle, story } = await create(client, 'lanes');
  const probe = await client.acquire(story.id, 'lanes-probe');
  const keepers = probe.acquisition.route.keepers.length;
  assert.equal(await probe.release(), true);
  const lanes = await client.acquireLanes(story.id, 'lanes-writer', 4, 1000000000n);
  try {
    assert.equal(lanes.lanes, Math.min(4, keepers));
    const results = [await lanes.append(payload(1)), ...await lanes.appendBatch([{ payload: payload(2) }, { payload: payload(3) }])];
    for (const result of results) assert.ok(!(result instanceof Error) && result.acked);
    results.sort((a, b) => a.hlc.physicalNs === b.hlc.physicalNs ? a.hlc.logical - b.hlc.logical : a.hlc.physicalNs < b.hlc.physicalNs ? -1 : 1);
    const { events } = await complete(client, story.id, results);
    assert.deepEqual(events.map(event => event.id), results.map(result => result.eventId));
    assert.deepEqual(events.map(event => Buffer.from(event.envelope.payload).toString()).sort(), [1, 2, 3].map(i => payload(i).toString()));
  } finally {
    assert.equal(await lanes.release(), true);
  }
  await assert.rejects(lanes.append(payload(4)), error => error instanceof FailedPrecondition && error.code === 'FAILED_PRECONDITION');
  await client.destroyStory(story.id);
  await client.destroyChronicle(chronicle);
});
