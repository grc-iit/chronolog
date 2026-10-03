const { test } = require('node:test');
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const { join } = require('node:path');
const { setTimeout: delay } = require('node:timers/promises');
const {
  connect, connectContext, encodeCheckpoint, decodeCheckpoint, rejectionOf, acquireRefusalOf, Durability,
  ChronologError, Cancelled, FailedPrecondition, InvalidArgument,
} = require('../dist');

const catalog = process.env.CHRONOLOG_TYPESCRIPT_CATALOG || '127.0.0.1:50051';
const player = process.env.CHRONOLOG_TYPESCRIPT_PLAYER || '127.0.0.1:50054';
const options = { catalog, player, rpcTimeoutMs: 8000 };
const max63 = (1n << 63n) - 1n;
const max64 = (1n << 64n) - 1n;
const text = value => ({ payload: Buffer.from(value), contentType: 'text/plain; charset=utf-8' });
const words = page => page.events.map(event => Buffer.from(event.envelope.payload).toString());
const show = value => JSON.stringify(value, (_, v) => typeof v === 'bigint' ? v.toString() : v);
const unique = name => `typescript-${name}-${process.pid}-${Date.now()}`;

// A cut may wait on a lagging writer; the native answer says when it is complete.
async function until(operation, done) {
  let last;
  for (let attempt = 0; attempt < 40; ++attempt) {
    last = await operation();
    if (done(last)) return last;
    await delay(100);
  }
  assert.fail(`answer never completed: ${show(last)}`);
}

test('checkpoint values round trip exactly at 2^63-1 and 2^64-1 and reject wider bigints', () => {
  const hlc = { physicalNs: max63, logical: 0xffffffff };
  const position = { hlc, id: { storyId: max64, writerId: max64, incarnation: max64, sequence: max64 } };
  const stamp = { writerId: max64, incarnation: max63 };
  const checkpoint = {
    identity: { agentId: 'agent', slot: 'slot' },
    context: { storyId: max64, chronicle: 'chronicle', name: 'context' },
    causalFloor: hlc,
    processedAfter: position,
    writer: stamp,
    acquisition: { hostId: 'host', launcherLockId: 'lock', expectedPriorIncarnation: max64, acquisitionRecordReceiptHlc: hlc },
    lastOwnReceiptHlc: hlc,
    recovery: {
      transitionId: 'transition', lowerBound: hlc,
      recoveredIncarnations: [{ writer: stamp, markerOperationId: 'marker' }, { writer: { writerId: 1n, incarnation: 1n } }],
      currentMarkerOperationId: 'marker', markerHlc: hlc,
    },
    unresolvedOperations: ['op-a'],
    permanentlyUnknownOperations: [{
      operationId: 'op-b', incarnations: [stamp], window: { start: { physicalNs: 0n, logical: 0 }, end: hlc },
      absenceProvable: true, normalizedDigest: Uint8Array.from([0, 1, 254, 255]),
    }],
    dispositions: [{ result: { operationId: 'op-c', outcome: 'LANDED', landed: position, observedDurability: Durability.DURABLE }, priorWriter: stamp }],
    priorStateUnknownBelow: hlc,
    acquisitionClosed: true,
    reconcileAttempted: true,
    takeoverRequired: false,
  };
  const encoded = encodeCheckpoint(checkpoint);
  assert.ok(encoded instanceof Uint8Array);
  const decoded = decodeCheckpoint(encoded);
  assert.deepEqual(decoded, checkpoint);
  assert.equal(decoded.causalFloor.physicalNs, max63);
  assert.equal(decoded.context.storyId, max64);
  assert.ok(Object.isFrozen(decoded) && Object.isFrozen(decoded.recovery.recoveredIncarnations[0]));
  assert.throws(() => encodeCheckpoint({ ...checkpoint, causalFloor: { physicalNs: max63 + 1n, logical: 0 } }), RangeError);
  assert.throws(() => encodeCheckpoint({ ...checkpoint, writer: { writerId: max64 + 1n, incarnation: 1n } }), RangeError);
  assert.throws(() => encodeCheckpoint({ ...checkpoint, causalFloor: { physicalNs: 1, logical: 0 } }), TypeError);
  assert.throws(() => encodeCheckpoint(checkpoint, 8), error => error instanceof ChronologError && error.code === 'RESOURCE_EXHAUSTED');
  assert.throws(() => decodeCheckpoint(Buffer.from('{}')), InvalidArgument);
});

test('Context round trip: remember, recall, latest, follow, fenced append, reconcile and close', { timeout: 55000 }, async () => {
  const contexts = await connectContext(options);
  const chronicle = unique('context');
  const ref = await contexts.ensureContext(chronicle, 'memory');
  assert.equal(typeof ref.storyId, 'bigint');
  assert.deepEqual(await contexts.ensureContext(chronicle, 'memory'), ref);
  assert.ok((await contexts.listContexts(chronicle)).some(item => item.storyId === ref.storyId));
  const identity = { agentId: 'ts-agent', slot: 'main' };
  const session = await contexts.open(ref, identity);
  assert.deepEqual(session.context, ref);
  assert.deepEqual(session.identity, identity);
  const first = session.status();
  assert.equal(first.state, 'READY');
  assert.equal(typeof first.writer.incarnation, 'bigint');

  const receipts = [];
  for (const word of ['alpha', 'beta', 'gamma']) {
    const result = await session.remember({ operationId: `op-${word}`, envelope: text(word) });
    assert.equal(result.current.outcome, 'DURABLE');
    assert.equal(result.current.status.code, 'OK');
    assert.equal(rejectionOf(result.current.status), 'UNSPECIFIED');
    assert.equal(result.current.receipt.acked, true);
    assert.ok(Object.isFrozen(result.current));
    receipts.push(result.current.receipt);
  }
  assert.deepEqual(receipts.map(r => r.eventId.sequence), [1n, 2n, 3n]);
  const retried = await session.remember({ operationId: 'op-alpha', envelope: text('alpha') });
  assert.deepEqual(retried.current.receipt.eventId, receipts[0].eventId);

  const page = await until(() => session.recall(), value => value.answerComplete);
  assert.deepEqual(words(page), ['alpha', 'beta', 'gamma']);
  assert.equal(page.streamStatus.code, 'OK');
  assert.equal(page.events[2].envelope.contentType, 'text/plain; charset=utf-8');
  const latest = await until(() => session.latest(2), value => value.selectionComplete);
  assert.deepEqual(words(latest.page), ['beta', 'gamma']);
  assert.equal(typeof latest.asOf.physicalNs, 'bigint');

  const controller = new AbortController();
  const followed = [];
  const subscription = contexts.subscribe([{ session, from: 'BEGINNING' }], { waitMs: 1000 }, { signal: controller.signal });
  while (followed.length < 3) {
    const { value } = await subscription.next();
    assert.equal(value.status.code, 'OK');
    followed.push(...value.pages[0].page.events);
  }
  assert.deepEqual(followed.map(e => e.id), receipts.map(r => r.eventId));
  const pending = subscription.next();
  await session.remember({ operationId: 'op-delta', envelope: text('delta') });
  const delivered = (await pending).value.pages[0];
  assert.deepEqual(words(delivered.page), ['delta']);
  const waiting = subscription.next();
  controller.abort();
  await assert.rejects(waiting, Cancelled);
  const idle = await contexts.follow([{ session, from: 'POSITION', after: delivered.resume }], { waitMs: 1 });
  assert.equal(idle.idle, true);
  assert.deepEqual(idle.pages[0].resume, delivered.resume);
  session.acknowledgeProcessed(delivered.resume);
  assert.deepEqual(session.checkpoint().processedAfter, delivered.resume);
  assert.throws(() => session.acknowledgeProcessed({ hlc: { physicalNs: 0n, logical: 0 }, id: receipts[0].eventId }), FailedPrecondition);

  // Another process-local Client takes the slot over, which fences the session's Writer.
  const raw = await connect(options);
  const taker = await raw.acquire(ref.storyId, `agent-context/v2:${JSON.stringify([identity.agentId, identity.slot])}`, { takeover: true });
  assert.ok(taker.acquisition.incarnation > first.writer.incarnation);
  const fenced = await session.remember({ operationId: 'op-fenced', envelope: text('fenced') });
  assert.equal(fenced.current.status.code, 'FAILED_PRECONDITION');
  assert.equal(rejectionOf(fenced.current.status), 'FENCED_SUPERSEDED');
  assert.equal(fenced.state, 'FENCED');
  assert.equal(session.status().state, 'FENCED');
  // C1: takeover first names the unknown newer holder as a typed PRIOR_MISMATCH, then CASes against exactly it.
  const named = await session.reconcile({ operationIds: ['op-fenced'], takeover: true });
  assert.equal(named.attempted, false, show(named));
  assert.equal(named.status.code, 'FAILED_PRECONDITION');
  assert.equal(acquireRefusalOf(named.status).refusalReason, 'PRIOR_MISMATCH');
  assert.equal(named.status.acquireRefusal.currentIncarnation, taker.acquisition.incarnation);
  const reconciled = await session.reconcile({ operationIds: ['op-fenced'], takeover: true });
  assert.equal(reconciled.attempted, true, show(reconciled));
  assert.equal(reconciled.status.code, 'OK', show(reconciled));
  assert.ok(reconciled.writer.incarnation > taker.acquisition.incarnation);
  const outcomes = Object.fromEntries(reconciled.operations.map(op => [op.operationId, op.outcome]));
  assert.ok(outcomes['op-fenced'] && outcomes['op-fenced'] !== 'LANDED', show(reconciled));
  for (const [id, outcome] of Object.entries(outcomes)) if (id !== 'op-fenced') assert.equal(outcome, 'LANDED', id);
  await assert.rejects(taker.append(Buffer.from('stale')), error =>
    error instanceof FailedPrecondition && rejectionOf(error) === 'FENCED_SUPERSEDED' && error.rejection === 'FENCED_SUPERSEDED');
  const after = await session.remember({ operationId: 'op-epsilon', envelope: text('epsilon') });
  assert.equal(after.current.outcome, 'DURABLE');
  assert.equal(after.current.receipt.eventId.incarnation, reconciled.writer.incarnation);
  const tail = await until(() => session.latest(1), value => value.selectionComplete);
  assert.deepEqual(words(tail.page), ['epsilon']);
  const closed = await session.close();
  assert.equal(closed.releaseCommitted, true);
  assert.equal(session.status().state, 'CLOSED');
});

test('AcquireOptions, process-owned request ids, typed CAS refusal and lease state', { timeout: 30000 }, async () => {
  const client = await connect(options);
  const chronicle = unique('lease');
  await client.createChronicle(chronicle);
  const story = await client.createStory(chronicle, 'events');
  const id = client.newAcquireRequestId();
  assert.equal(typeof id, 'string');
  assert.notEqual(id, client.newAcquireRequestId());
  await assert.rejects(client.acquire(story.id, 'lease-writer', { acquireRequestId: 'not-issued-here' }), InvalidArgument);
  const writer = await client.acquire(story.id, 'lease-writer', { acquireRequestId: id, leaseDurationNs: 30000000000n });
  assert.equal(typeof writer.acquisition.lease.durationNs, 'bigint');
  assert.ok(writer.acquisition.lease.durationNs > 0n);
  const lease = writer.lease();
  assert.deepEqual(lease.grant, writer.acquisition.lease);
  assert.equal(lease.confirmed, true);
  assert.equal(typeof lease.renewals, 'bigint');
  assert.equal(lease.terminationCause, undefined);
  const other = await connect(options);
  const refused = await other.acquire(story.id, 'lease-writer', { expectedPriorIncarnation: 7n, takeover: true }).then(
    () => assert.fail('a mismatched prior must refuse'), error => error);
  assert.ok(refused instanceof FailedPrecondition);
  assert.equal(acquireRefusalOf(refused).refusalReason, 'PRIOR_MISMATCH');
  assert.equal(refused.acquireRefusal.currentIncarnation, writer.acquisition.incarnation);
  assert.equal(rejectionOf(refused), 'UNSPECIFIED');
  await assert.rejects(client.acquire(story.id, 'lease-writer', { expectedPriorIncarnation: 0n }), InvalidArgument);
  const aborted = new AbortController();
  aborted.abort();
  await assert.rejects(client.acquire(story.id, 'lease-writer', { signal: aborted.signal }), Cancelled);
  assert.equal(await writer.release(), true);
});

test('dropping a live ContextClient with a follow open lets the process exit cleanly', { timeout: 40000 }, () => {
  for (const mode of ['drain', 'exit']) {
    const child = spawnSync(process.execPath, ['--expose-gc', join(__dirname, 'finalizer_child.js'), mode],
      { env: process.env, encoding: 'utf8', timeout: 30000 });
    assert.equal(child.error, undefined, `${mode}: ${child.error}`);
    assert.equal(child.signal, null, `${mode}: ${child.stderr}`);
    assert.equal(child.status, 0, `${mode}: ${child.stderr}`);
    assert.match(child.stdout, /collected/, mode);
    assert.equal(child.stderr, '', mode);
  }
});
