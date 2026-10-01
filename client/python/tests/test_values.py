from dataclasses import FrozenInstanceError

import pytest
import chronolog as cl


def test_binary_and_immutable_values():
    attrs = {"gen_ai.operation.name": "chat"}
    e = cl.Envelope(b"\x00\xff", attributes=attrs, trace_id=b"t" * 16, span_id=b"s" * 8)
    attrs["gen_ai.operation.name"] = "changed"
    e.attributes["gen_ai.operation.name"] = "changed again"
    assert e.attributes == {"gen_ai.operation.name": "chat"}
    assert e == cl.Envelope(b"\x00\xff", attributes=e.attributes, trace_id=b"t" * 16, span_id=b"s" * 8)
    for obj, field in [(e, "payload"), (cl.Hlc(5), "logical"), (cl.EventId(), "sequence"),
                       (cl.Route(), "epoch"), (cl.Completion(), "complete")]:
        assert type(obj).__name__ in repr(obj)
        with pytest.raises(FrozenInstanceError):
            setattr(obj, field, 1)
    assert not cl.AppendResult(cl.EventId(), cl.Hlc(), cl.Durability.ACCEPTED).acked
    assert cl.AppendResult(cl.EventId(), cl.Hlc(), cl.Durability.DURABLE).acked


def test_status_translation_and_expected_sequence():
    error = cl._error(9, "expected sequence 17")
    assert isinstance(error, cl.FailedPrecondition)
    assert error.expected_sequence == error.status.expected_sequence == 17
    for code, cls in [(1, cl.Cancelled), (3, cl.InvalidArgument), (4, cl.DeadlineExceeded),
                      (5, cl.NotFound), (11, cl.OutOfRange), (12, cl.Unimplemented), (14, cl.Unavailable)]:
        assert isinstance(cl._error(code, "failure"), cls)
        assert cl._error(code, "failure").status.code == code


def test_deadline_validation():
    for timeout in [0, -1, float("nan"), float("inf")]:
        with pytest.raises(ValueError):
            cl.connect("127.0.0.1:1", timeout=timeout)
