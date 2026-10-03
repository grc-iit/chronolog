from dataclasses import FrozenInstanceError, replace

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


MAX63 = 2**63 - 1
MAX64 = 2**64 - 1


def full_checkpoint():
    hlc = cl.Hlc(MAX63, 2**32 - 1)
    position = cl.Position(hlc, cl.EventId(MAX64, MAX64, MAX64, MAX64))
    stamp = cl.WriterStamp(MAX64, MAX63)
    return cl.Checkpoint(
        identity=cl.AgentIdentity("agent", "slot"),
        context=cl.ContextRef(MAX64, "chronicle", "context"),
        causal_floor=hlc,
        processed_after=position,
        writer=stamp,
        acquisition=cl.AcquisitionProvenance("host", "lock", MAX64, hlc),
        last_own_receipt_hlc=hlc,
        recovery=cl.ReconcileCheckpoint("transition", hlc, [cl.RecoveryIncarnation(stamp, "marker"),
                                                            cl.RecoveryIncarnation(cl.WriterStamp(1, 1))],
                                        "marker", hlc),
        unresolved_operations=["op-a"],
        permanently_unknown_operations=[cl.UnknownOperation("op-b", [stamp], cl.HlcRange(cl.Hlc(), hlc), True,
                                                            bytes([0, 1, 254, 255]))],
        dispositions=[cl.OperationDisposition(cl.ReconciledOperation("op-c", cl.ReconcileOutcome.LANDED, position,
                                                                     cl.Durability.DURABLE), stamp)],
        prior_state_unknown_below=hlc,
        acquisition_closed=True,
        reconcile_attempted=True,
    )


def test_checkpoint_round_trips_exact_ints_and_rejects_wider_ones():
    checkpoint = full_checkpoint()
    encoded = cl.encode_checkpoint(checkpoint)
    assert isinstance(encoded, bytes)
    decoded = cl.decode_checkpoint(encoded)
    assert decoded == checkpoint
    assert decoded.causal_floor.physical_ns == MAX63 and type(decoded.causal_floor.physical_ns) is int
    assert decoded.context.story_id == MAX64 and decoded.writer.writer_id == MAX64
    assert decoded.dispositions[0].result.outcome is cl.ReconcileOutcome.LANDED
    with pytest.raises(FrozenInstanceError):
        decoded.recovery.recovered_incarnations[0].writer = None
    with pytest.raises(OverflowError):
        cl.encode_checkpoint(replace(checkpoint, causal_floor=cl.Hlc(MAX63 + 1)))
    with pytest.raises(OverflowError):
        cl.encode_checkpoint(replace(checkpoint, writer=cl.WriterStamp(MAX64 + 1, 1)))
    with pytest.raises(OverflowError):
        cl.encode_checkpoint(replace(checkpoint, writer=cl.WriterStamp(-1, 1)))
    with pytest.raises(TypeError):
        cl.encode_checkpoint(replace(checkpoint, causal_floor=cl.Hlc(1.0)))
    with pytest.raises(cl.ResourceExhausted) as small:
        cl.encode_checkpoint(checkpoint, 8)
    assert small.value.status.code == cl.StatusCode.RESOURCE_EXHAUSTED
    with pytest.raises(cl.InvalidArgument):
        cl.decode_checkpoint(b"{}")


def test_typed_rejection_and_refusal_details():
    fenced = cl._error(9, "fenced", cl.AppendRejection.FENCED_SUPERSEDED)
    assert isinstance(fenced, cl.FailedPrecondition)
    assert cl.rejection_of(fenced) is cl.AppendRejection.FENCED_SUPERSEDED
    assert cl.rejection_of(fenced.status) is cl.AppendRejection.FENCED_SUPERSEDED
    assert cl.acquire_refusal_of(fenced) is None
    refusal = cl.AcquireRefusal(cl.AcquireRefusalReason.PRIOR_MISMATCH, current_incarnation=MAX64)
    refused = cl._error(9, "prior mismatch", 0, refusal)
    assert cl.acquire_refusal_of(refused).current_incarnation == MAX64
    assert cl.rejection_of(refused) is cl.AppendRejection.UNSPECIFIED
    assert cl._status(0, "").ok and cl._status(0, "").code is cl.StatusCode.OK
    for code, cls in [(7, cl.PermissionDenied), (10, cl.Aborted), (13, cl.Internal), (15, cl.DataLoss),
                      (16, cl.Unauthenticated), (2, cl.Error)]:
        assert type(cl._error(code, "x")) is cls
