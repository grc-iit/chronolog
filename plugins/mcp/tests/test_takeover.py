import asyncio
import json
import os

import chronolog as cl
import pytest

from _mcp import Mcp, launcher, raw_client, unique, until

pytestmark = pytest.mark.skipif(not os.getenv("CHRONOLOG_TEST_VISOR"), reason="real stack required")


def test_foreign_host_unclosed_records_require_deliberate_takeover(tmp_path):
    async def run():
        chronicle, identity = unique("foreign"), unique("planner")
        async with Mcp(launcher(str(tmp_path), identity, chronicle, host="host-a", lock="a")) as mcp:
            opened = await mcp.call("context_open", name="shared", create=True)
            await mcp.call("context_remember", session_handle=opened["session_handle"], operation_id="a1",
                           content="from a")
            mcp.kill()
        async with Mcp(launcher(str(tmp_path), identity, chronicle, host="host-b", lock="b")) as mcp:
            status = await mcp.call("context_status")
            assert status["checkpoint_store"]["state"] == "takeover_required", status
            refused = await mcp.call("context_open", name="shared")
            assert refused["session_handle"] is None and refused["store_state"] == "takeover_required", refused
            assert "takeover=true" in refused["next_action"]
            # Reads continue without the checkpoint store.
            reader = await mcp.call("context_open", name="shared", access="read_only")
            read = await until(lambda: mcp.call("context_recall", session_handle=reader["session_handle"]),
                               lambda r: r["answer_complete"])
            assert [e["content"]["data"] for e in read["events"]] == ["from a"]
            declined = await mcp.call("context_reconcile")
            assert not declined["attempted"] and declined["store_state"] == "takeover_required"
            store = await mcp.call("context_reconcile", takeover=True)
            assert store["attempted"] and store["store"]["state"] == "ready", store
            fenced = await mcp.call("context_open", name="shared")
            assert fenced["state"] == "FENCED" and fenced["takeover_required"], fenced
            handle = fenced["session_handle"]
            blocked = await mcp.call("context_remember", session_handle=handle, operation_id="b1", content="from b")
            assert blocked["stored"] == "rejected" and "takeover" in blocked["next_action"]
            # b1 has a seen outcome (rejected before dispatch), so it is not passed to reconcile.
            plain = await mcp.call("context_reconcile", session_handle=handle, operation_ids=[])
            assert not plain["attempted"] and plain["status"]["code"] == "FAILED_PRECONDITION", plain
            taken = await mcp.call("context_reconcile", session_handle=handle, operation_ids=[], takeover=True)
            assert taken["attempted"] and taken["state"] == "READY", taken
            assert int(taken["writer"]["incarnation"]) > int(opened["writer"]["incarnation"])
            stored = await mcp.call("context_remember", session_handle=handle, operation_id="b1", content="from b")
            assert stored["stored"] == "durable"

    asyncio.run(asyncio.wait_for(run(), timeout=50))


def test_fenced_control_writer_fails_writable_tools_closed_and_reads_continue(tmp_path):
    async def run():
        chronicle, identity = unique("fenced"), unique("planner")
        async with Mcp(launcher(str(tmp_path), identity, chronicle)) as mcp:
            opened = await mcp.call("context_open", name="notes", create=True)
            handle = opened["session_handle"]
            await mcp.call("context_remember", session_handle=handle, operation_id="n1", content="note")
            store = (await mcp.call("context_status"))["checkpoint_store"]
            control = f"agent-context-control/v2:{json.dumps([identity, 'checkpoints'], separators=(',', ':'))}"
            raw_client().acquire(int(store["story"]["story_id"]), control, options=cl.AcquireOptions(takeover=True))
            failed = await mcp.call("context_checkpoint", session_handle=handle)
            assert failed["stored"] == "failed" and "checkpoint_store_fenced" in failed["verdict"], failed
            assert control.split(":", 1)[1] in failed["verdict"]
            refused = await mcp.call("context_remember", session_handle=handle, operation_id="n2", content="no")
            assert refused["stored"] == "rejected" and refused["store_state"] == "checkpoint_store_fenced"
            other = await mcp.call("context_open", name="other", create=True)
            assert other["session_handle"] is None and other["store_state"] == "checkpoint_store_fenced"
            read = await until(lambda: mcp.call("context_recall", session_handle=handle),
                               lambda r: r["answer_complete"])
            assert [e["content"]["data"] for e in read["events"]] == ["note"]
            assert (await mcp.call("context_list"))["answer_complete"]

    asyncio.run(asyncio.wait_for(run(), timeout=50))
