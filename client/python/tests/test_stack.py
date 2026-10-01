import concurrent.futures
import os
import threading
import time
import uuid

import pytest
import chronolog as cl

pytestmark = pytest.mark.skipif(not os.environ.get("CHRONOLOG_TEST_VISOR"), reason="real stack required")


@pytest.fixture
def stack():
    client = cl.connect(os.environ["CHRONOLOG_TEST_VISOR"], os.environ.get("CHRONOLOG_TEST_PLAYER"), timeout=5)
    chronicle = client.create_chronicle(f"pyclient-{uuid.uuid4().hex}")
    story = client.create_story(chronicle, "events")
    try:
        ready_by = time.monotonic() + 15
        while True:
            try:
                list(client.read(story, end=cl.Hlc(1), timeout=2))
                break
            except cl.FailedPrecondition:
                if time.monotonic() >= ready_by:
                    raise
                time.sleep(0.1)
        yield client, chronicle, story
    finally:
        client.destroy_story(story, timeout=5)
        client.destroy_chronicle(chronicle, timeout=5)


def test_catalog_append_read_tail_and_fencing(stack):
    client, chronicle, story = stack
    assert client.chronicle(chronicle) == chronicle
    assert chronicle in client.list_chronicles()
    assert client.story(story) == story
    assert story in client.list_stories(chronicle)
    with client.acquire(story, "writer") as writer:
        with pytest.raises(cl.InvalidArgument) as invalid:
            writer.append(b"bad", trace_id=b"bad", timeout=3)
        assert invalid.value.status.code == 3
        first = writer.append(b"\x00\xff", content_type="application/octet-stream", attributes={"test": "sdk"},
                              trace_id=b"t" * 16, span_id=b"s" * 8, timeout=3)
        rest = writer.append_batch([b"two", cl.Envelope(b"three")], timeout=3)
        assert first.event_id.sequence == 1 and first.acked
        assert [r.event_id.sequence for r in rest] == [2, 3]
        assert all(r.acked for r in rest)
        end = cl.Hlc(rest[-1].hlc.physical_ns, rest[-1].hlc.logical + 1)
        deadline = time.monotonic() + 15
        while True:
            reader = client.read(story, first.hlc, end, timeout=3)
            events = list(reader)
            if reader.completion.complete:
                break
            assert time.monotonic() < deadline
            time.sleep(0.1)
        assert [e.envelope.payload for e in events] == [b"\x00\xff", b"two", b"three"]
        assert [e.hlc for e in events] == [first.hlc, *[r.hlc for r in rest]]
        assert events[0].envelope.attributes == {"test": "sdk"}
        assert events[0].envelope.trace_id == b"t" * 16
        assert events[0].envelope.span_id == b"s" * 8
        assert reader.continuation is None
        tail = client.tail(story, events[0], timeout=3)
        assert next(tail).id == events[1].id
        tail.cancel()
        tail.cancel()
        assert list(tail) == []
    with pytest.raises(cl.FailedPrecondition):
        writer.append(b"released", timeout=3)
    with client.acquire(story, "writer") as again:
        assert again.writer_id == writer.writer_id
        assert again.incarnation == writer.incarnation + 1
        assert again.append(b"new", timeout=3).event_id.sequence == 1


def test_failed_batch_keeps_item_order_and_sequence(stack):
    client, _, story = stack
    with client.acquire(story, "batch") as writer:
        results = writer.append_batch([b"ok", cl.Envelope(b"invalid", span_id=b"bad"), b"gap"], timeout=3)
        assert results[0].event_id.sequence == 1
        assert isinstance(results[1], cl.InvalidArgument)
        assert isinstance(results[2], cl.FailedPrecondition)
        assert results[2].expected_sequence == 2
        assert writer.append(b"repair", timeout=3).event_id.sequence == 2


def test_blocked_tail_releases_gil_and_cancel_unblocks(stack):
    client, _, story = stack
    with client.acquire(story, "gil") as writer:
        tail = client.tail(story, timeout=5)
        entered = threading.Event()

        def pull():
            entered.set()
            return next(tail)

        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            pending = pool.submit(pull)
            assert entered.wait(1)
            time.sleep(0.2)
            assert not pending.done()
            append = pool.submit(writer.append, b"while blocked", timeout=2)
            result = append.result(timeout=3)
            assert pending.result(timeout=3).id == result.event_id
            blocked = pool.submit(pull)
            time.sleep(0.2)
            tail.cancel()
            with pytest.raises(StopIteration):
                blocked.result(timeout=2)
