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


def test_kind_actor_and_links_round_trip(stack):
    client, _, story = stack
    with client.acquire(story, "envelope-fields") as writer:
        first = writer.append(b"one", timeout=3)
        dangling = cl.EventId(story.id, 99, 1, 7)
        links = (cl.Link("replies_to", first.event_id, first.hlc), cl.Link("cites", dangling))
        second = writer.append(b"two", kind="note.reply", actor="agent:py-test", links=links, timeout=3)
        end = cl.Hlc(second.hlc.physical_ns, second.hlc.logical + 1)
        for attempt in range(30):
            reader = client.read(story, first.hlc, end, timeout=3)
            events = list(reader)
            if reader.completion.complete:
                break
            time.sleep(0.1)
        assert reader.completion.complete and len(events) == 2
        assert (events[0].envelope.kind, events[0].envelope.actor, events[0].envelope.links) == ("", "", ())
        assert events[1].envelope.kind == "note.reply"
        assert events[1].envelope.actor == "agent:py-test"
        assert events[1].envelope.links == links
        assert events[1].envelope.links[1].target_hlc is None
    with pytest.raises(TypeError):
        cl.Envelope(b"x", links=[(first.event_id, "replies_to")])


def test_default_read_and_tail_deliver_eight_maximal_payloads(stack):
    client, _, story = stack
    payloads = [bytes([i]) * (1024 * 1024) for i in range(8)]
    with client.acquire(story, "receive-limit") as writer:
        results = writer.append_batch(payloads, timeout=5)
        assert len(results) == 8
        assert all(isinstance(result, cl.AppendResult) and result.acked for result in results)
        end = cl.Hlc(results[-1].hlc.physical_ns, results[-1].hlc.logical + 1)
        for attempt in range(30):
            reader = client.read(story, results[0].hlc, end, timeout=5)
            events = list(reader)
            if reader.completion.complete:
                break
            time.sleep(0.1)
        assert reader.completion.complete
        assert [event.id for event in events] == [result.event_id for result in results]
        assert [event.envelope.payload for event in events] == payloads
        tail = client.tail(story, timeout=5)
        try:
            tailed = [next(tail) for _ in range(8)]
            assert [event.id for event in tailed] == [result.event_id for result in results]
            assert [event.envelope.payload for event in tailed] == payloads
        finally:
            tail.cancel()


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


def test_physical_read_discovers_route_and_out_of_range_consumes_sequence(stack):
    _, _, story = stack
    client = cl.connect(os.environ["CHRONOLOG_TEST_VISOR"], timeout=5)
    with client.acquire(story, "physical") as writer:
        with pytest.raises(cl.OutOfRange):
            writer.append(b"too-old", physical=cl.TimeReading(1), timeout=3)
        result = writer.append(b"physical-event", timeout=3)
        assert result.event_id.sequence == 2
        stream = client.read_physical(story, 0, time.time_ns() + 1_000_000_000, timeout=3)
        events = list(stream)
        assert len(events) == 1 and events[0].id == result.event_id
        assert not stream.completion.complete


def test_lane_writer_spreads_alternating_slices_and_reads_back_once(stack):
    client, _, story = stack
    slice_ns = 1_000_000_000
    first = time.time_ns() // slice_ns * slice_ns - 3 * slice_ns
    readings = [cl.TimeReading(first + k * slice_ns + slice_ns // 2) for k in range(6)]
    with client.acquire_lanes(story, "lanes", 2, slice_ns, timeout=5) as lanes:
        assert 1 <= lanes.lanes <= 2
        results = [lanes.append(f"e{k}".encode(), physical=readings[k], timeout=3) for k in range(3)]
        results += lanes.append_batch([cl.AppendSpec(cl.Envelope(f"e{k}".encode()), physical=readings[k])
                                       for k in range(3, 6)], timeout=3)
        assert all(isinstance(r, cl.AppendResult) and r.acked for r in results)
        assert len({r.event_id.writer_id for r in results}) == lanes.lanes
        end = max(r.hlc for r in results)
        end = cl.Hlc(end.physical_ns, end.logical + 1)
        deadline = time.monotonic() + 15
        while True:
            reader = client.read(story, end=end, timeout=3)
            events = list(reader)
            if reader.completion.complete:
                break
            assert time.monotonic() < deadline
            time.sleep(0.1)
    assert len(events) == 6 and {e.id for e in events} == {r.event_id for r in results}
    assert sorted(e.envelope.payload for e in events) == [f"e{k}".encode() for k in range(6)]
    with pytest.raises(cl.FailedPrecondition):
        lanes.append(b"released", timeout=3)
