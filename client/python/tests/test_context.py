import json
import os
import subprocess
import sys
import time
import uuid

import pytest
import chronolog as cl

pytestmark = pytest.mark.skipif(not os.environ.get("CHRONOLOG_TEST_VISOR"), reason="real stack required")


def options():
    return cl.ContextOptions(os.environ["CHRONOLOG_TEST_VISOR"], os.environ.get("CHRONOLOG_TEST_PLAYER"), timeout=8)


def unique(name):
    return f"python-{name}-{uuid.uuid4().hex}"


def text(word):
    return cl.Envelope(word.encode(), content_type="text/plain; charset=utf-8")


def words(page):
    return [e.envelope.payload.decode() for e in page.events]


# A cut may wait on a lagging writer; the native answer says when it is complete.
def until(operation, done):
    for _ in range(40):
        last = operation()
        if done(last):
            return last
        time.sleep(0.1)
    pytest.fail(f"answer never completed: {last}")


def test_context_round_trip_fenced_append_and_reconcile():
    contexts = cl.connect_context(options())
    chronicle = unique("context")
    ref = contexts.ensure_context(chronicle, "memory")
    assert type(ref.story_id) is int
    assert contexts.ensure_context(chronicle, "memory") == ref
    assert ref in contexts.list_contexts(chronicle)
    identity = cl.AgentIdentity("py-agent", "main")
    session = contexts.open(ref, identity)
    assert session.context == ref and session.identity == identity
    first = session.status()
    assert first.state is cl.SessionState.READY

    receipts = []
    for word in ["alpha", "beta", "gamma"]:
        result = session.remember(cl.Memory(f"op-{word}", text(word)))
        assert result.current.outcome is cl.MemoryOutcome.DURABLE
        assert result.current.status.ok
        assert cl.rejection_of(result.current.status) is cl.AppendRejection.UNSPECIFIED
        assert result.current.receipt.acked
        receipts.append(result.current.receipt)
    assert [r.event_id.sequence for r in receipts] == [1, 2, 3]
    retried = session.remember(cl.Memory("op-alpha", text("alpha")))
    assert retried.current.receipt.event_id == receipts[0].event_id

    page = until(session.recall, lambda p: p.answer_complete)
    assert words(page) == ["alpha", "beta", "gamma"]
    assert page.stream_status.ok
    assert page.events[2].envelope.content_type == "text/plain; charset=utf-8"
    latest = until(lambda: session.latest(2), lambda r: r.selection_complete)
    assert words(latest.page) == ["beta", "gamma"]
    assert type(latest.as_of.physical_ns) is int

    subscription = contexts.subscribe([cl.FollowInput(session, cl.FollowFrom.BEGINNING)],
                                      options=cl.FollowOptions(wait=1))
    followed = []
    while len(followed) < 3:
        result = next(subscription)
        assert result.status.ok
        followed.extend(result.pages[0].page.events)
    assert [e.id for e in followed] == [r.event_id for r in receipts]
    session.remember(cl.Memory("op-delta", text("delta")))
    delivered = next(subscription).pages[0]
    assert words(delivered.page) == ["delta"]
    subscription.close()
    idle = contexts.follow([cl.FollowInput(session, cl.FollowFrom.POSITION, delivered.resume)],
                           options=cl.FollowOptions(wait=0.001))
    assert idle.idle and idle.pages[0].resume == delivered.resume
    session.acknowledge_processed(delivered.resume)
    assert session.checkpoint().processed_after == delivered.resume
    with pytest.raises(cl.FailedPrecondition):
        session.acknowledge_processed(cl.Position(cl.Hlc(), receipts[0].event_id))

    # Another process-local Client takes the slot over, which fences the session's Writer.
    raw = cl.connect(os.environ["CHRONOLOG_TEST_VISOR"], timeout=8)
    slot = json.dumps([identity.agent_id, identity.slot], separators=(",", ":"))
    taker = raw.acquire(ref.story_id, f"agent-context/v2:{slot}", options=cl.AcquireOptions(takeover=True))
    assert taker.incarnation > first.writer.incarnation
    fenced = session.remember(cl.Memory("op-fenced", text("fenced")))
    assert fenced.current.status.code is cl.StatusCode.FAILED_PRECONDITION
    assert cl.rejection_of(fenced.current.status) is cl.AppendRejection.FENCED_SUPERSEDED
    assert fenced.state is cl.SessionState.FENCED
    assert session.status().state is cl.SessionState.FENCED
    # Takeover first names the unknown newer holder as a typed PRIOR_MISMATCH, then CASes against exactly it.
    named = session.reconcile(options=cl.ReconcileOptions(["op-fenced"], takeover=True))
    assert not named.attempted, named
    assert named.status.code is cl.StatusCode.FAILED_PRECONDITION
    assert cl.acquire_refusal_of(named.status).refusal_reason is cl.AcquireRefusalReason.PRIOR_MISMATCH
    assert named.status.acquire_refusal.current_incarnation == taker.incarnation
    reconciled = session.reconcile(options=cl.ReconcileOptions(["op-fenced"], takeover=True))
    assert reconciled.attempted and reconciled.status.ok, reconciled
    assert reconciled.writer.incarnation > taker.incarnation
    outcomes = {op.operation_id: op.outcome for op in reconciled.operations}
    assert outcomes["op-fenced"] is not cl.ReconcileOutcome.LANDED, reconciled
    assert all(outcome is cl.ReconcileOutcome.LANDED for op, outcome in outcomes.items() if op != "op-fenced")
    with pytest.raises(cl.FailedPrecondition) as stale:
        taker.append(b"stale", timeout=5)
    assert cl.rejection_of(stale.value) is cl.AppendRejection.FENCED_SUPERSEDED
    assert stale.value.rejection is cl.AppendRejection.FENCED_SUPERSEDED
    after = session.remember(cl.Memory("op-epsilon", text("epsilon")))
    assert after.current.outcome is cl.MemoryOutcome.DURABLE
    assert after.current.receipt.event_id.incarnation == reconciled.writer.incarnation
    tail = until(lambda: session.latest(1), lambda r: r.selection_complete)
    assert words(tail.page) == ["epsilon"]
    assert session.close().release_committed
    assert session.status().state is cl.SessionState.CLOSED


def test_acquire_options_request_ids_typed_refusal_and_lease():
    client = cl.connect(os.environ["CHRONOLOG_TEST_VISOR"], timeout=8)
    chronicle = client.create_chronicle(unique("lease"))
    story = client.create_story(chronicle, "events")
    request = client.new_acquire_request_id()
    assert isinstance(request, str) and request != client.new_acquire_request_id()
    with pytest.raises(cl.InvalidArgument):
        client.acquire(story, "lease-writer", options=cl.AcquireOptions(acquire_request_id="not-issued-here"))
    writer = client.acquire(story, "lease-writer",
                            options=cl.AcquireOptions(lease_duration_ns=30_000_000_000, acquire_request_id=request))
    assert type(writer.acquisition.lease.duration_ns) is int and writer.acquisition.lease.duration_ns > 0
    lease = writer.lease()
    assert lease.grant == writer.acquisition.lease
    assert lease.confirmed and type(lease.renewals) is int and lease.termination_cause is None
    other = cl.connect(os.environ["CHRONOLOG_TEST_VISOR"], timeout=8)
    with pytest.raises(cl.FailedPrecondition) as refused:
        other.acquire(story, "lease-writer", options=cl.AcquireOptions(takeover=True, expected_prior_incarnation=7))
    assert cl.acquire_refusal_of(refused.value).refusal_reason is cl.AcquireRefusalReason.PRIOR_MISMATCH
    assert refused.value.acquire_refusal.current_incarnation == writer.incarnation
    assert cl.rejection_of(refused.value) is cl.AppendRejection.UNSPECIFIED
    with pytest.raises(cl.InvalidArgument):
        client.acquire(story, "lease-writer", options=cl.AcquireOptions(expected_prior_incarnation=0))
    with pytest.raises(OverflowError):
        client.acquire(story, "lease-writer", options=cl.AcquireOptions(lease_duration_ns=2**63))
    assert writer.release()


def test_forked_child_refuses_inherited_handles():
    client = cl.connect(os.environ["CHRONOLOG_TEST_VISOR"], timeout=8)
    story = client.create_story(client.create_chronicle(unique("fork")), "events")
    writer = client.acquire(story, "parent")
    contexts = cl.connect_context(options())
    session = contexts.open(contexts.ensure_context(unique("fork"), "memory"), cl.AgentIdentity("parent", "main"))
    calls = [lambda: client.list_chronicles(timeout=2), lambda: client.acquire(story, "child", timeout=2),
             client.new_acquire_request_id, lambda: writer.append(b"child", timeout=2),
             lambda: client.tail(story, timeout=2), lambda: contexts.ensure_context("x", "y", timeout=2),
             lambda: session.remember(cl.Memory("child", text("child")), timeout=2), session.status]
    read, write = os.pipe()
    pid = os.fork()
    if pid == 0:
        os.close(read)
        failures = []
        for i, use in enumerate(calls):
            try:
                use()
                failures.append(f"{i}: no error")
            except cl.FailedPrecondition as error:
                if "after fork" not in str(error):
                    failures.append(f"{i}: {error}")
            except BaseException as error:  # noqa: B902
                failures.append(f"{i}: {type(error).__name__} {error}")
        os.write(write, "\n".join(failures).encode())
        os._exit(0)
    os.close(write)
    deadline = time.monotonic() + 20
    while (done := os.waitpid(pid, os.WNOHANG))[0] == 0:
        assert time.monotonic() < deadline, "forked child hung"
        time.sleep(0.05)
    with os.fdopen(read, "rb") as pipe:
        report = pipe.read().decode()
    assert os.waitstatus_to_exitcode(done[1]) == 0
    assert report == ""
    # The parent's handles are untouched by the child.
    assert writer.append(b"parent", timeout=5).event_id.sequence == 1
    assert session.remember(cl.Memory("parent", text("parent"))).current.outcome is cl.MemoryOutcome.DURABLE
    assert writer.release()


def test_dropping_a_live_context_client_with_an_open_follow_exits_cleanly():
    child = os.path.join(os.path.dirname(__file__), "finalizer_child.py")
    for mode in ["drain", "exit"]:
        run = subprocess.run([sys.executable, child, mode], capture_output=True, text=True, timeout=30)
        assert run.returncode == 0, f"{mode}: {run.stderr}"
        assert "collected idle session" in run.stdout, mode
        # In "exit" a daemon thread inside the native follow still holds the session when the interpreter exits.
        assert ("collected drain session" in run.stdout) == (mode == "drain"), mode
        assert run.stderr == "", mode
