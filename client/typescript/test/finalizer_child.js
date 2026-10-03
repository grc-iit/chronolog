// Drops a live ContextClient while its follow is still open. "drain" lets the event loop empty; "exit" calls
// process.exit while the native follow thread is still waiting.
const { connectContext } = require('../dist');

const options = {
  catalog: process.env.CHRONOLOG_TYPESCRIPT_CATALOG || '127.0.0.1:50051',
  player: process.env.CHRONOLOG_TYPESCRIPT_PLAYER || '127.0.0.1:50054',
  rpcTimeoutMs: 8000,
};
const mode = process.argv[2];
const collected = new FinalizationRegistry(label => process.stdout.write(`collected ${label}\n`));

async function abandon(label) {
  const contexts = await connectContext(options);
  const ref = await contexts.ensureContext(`typescript-finalizer-${process.pid}-${Date.now()}`, label);
  const session = await contexts.open(ref, { agentId: 'finalizer', slot: label });
  const result = await session.remember({ operationId: label, envelope: { payload: Buffer.from(label) } });
  if (result.current.outcome !== 'DURABLE') throw new Error(`remember ${result.current.outcome}`);
  const follow = contexts.follow([{ session, from: 'POSITION', after: { hlc: result.current.receipt.hlc, id: result.current.receipt.eventId } }], { waitMs: 3000 });
  follow.catch(error => { process.stderr.write(`follow failed: ${error}\n`); process.exitCode = 1; });
  collected.register(session, `${label} session`);
  return { follow };
}

(async () => {
  // A second client whose wrappers become garbage at once: its finalizers reap a live Writer off the JS thread.
  await (async () => {
    const contexts = await connectContext(options);
    const ref = await contexts.ensureContext(`typescript-finalizer-${process.pid}-${Date.now()}`, 'idle');
    collected.register(await contexts.open(ref, { agentId: 'finalizer', slot: 'idle' }), 'idle session');
  })();
  await abandon(mode);
  for (let i = 0; i < 3; ++i) {
    global.gc();
    await new Promise(resolve => setImmediate(resolve));
  }
  if (mode === 'exit') process.exit(0);
})().catch(error => { process.stderr.write(`${error.stack}\n`); process.exitCode = 1; });
