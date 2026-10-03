import asyncio
import os
import time

import pytest

from _mcp import Mcp, launcher, operation_ids, unique, until

pytestmark = pytest.mark.skipif(not os.getenv("CHRONOLOG_TEST_VISOR"), reason="real stack required")


def test_crashed_server_restart_reconciles_and_the_retried_operation_is_landed_once(tmp_path):
    async def run():
        chronicle, identity = unique("crash"), unique("worker")
        args = launcher(str(tmp_path), identity, chronicle)
        async with Mcp(args) as mcp:
            opened = await mcp.call("context_open", name="ops", create=True)
            handle, story = opened["session_handle"], int(opened["context"]["story_id"])
            await mcp.call("context_remember", session_handle=handle, operation_id="op-0", content="before")
            # Kill the server once op-1 is stored, whether or not its answer was sent: the agent saw no outcome.
            pending = asyncio.create_task(mcp.call("context_remember", session_handle=handle, operation_id="op-1",
                                                   content="in flight"))
            await until(lambda: asyncio.to_thread(operation_ids, story), lambda ids: "op-1" in ids, "op-1 storage")
            mcp.kill()
            pending.cancel()
        async with Mcp(args) as mcp:
            status = await mcp.call("context_status")
            assert status["checkpoint_store"]["state"] == "ready", status
            reopened = await mcp.call("context_open", name="ops")
            assert reopened["state"] == "NEEDS_RECONCILE" and not reopened["takeover_required"], reopened
            handle = reopened["session_handle"]
            blocked = await mcp.call("context_remember", session_handle=handle, operation_id="op-2", content="next")
            assert blocked["stored"] == "rejected" and blocked["next_action"] == "context_reconcile", blocked
            reconciled = await mcp.call("context_reconcile", session_handle=handle, operation_ids=["op-1"])
            assert reconciled["attempted"] and reconciled["checkpoint_persisted"] == "durable", reconciled
            assert reconciled["supply_all_unseen_operation_ids"] and reconciled["omitted_operation_ids_may_duplicate"]
            assert {op["operation_id"]: op["outcome"] for op in reconciled["operations"]}["op-1"] == "LANDED"
            assert int(reconciled["writer"]["incarnation"]) > int(opened["writer"]["incarnation"])
            retried = await mcp.call("context_remember", session_handle=handle, operation_id="op-1",
                                     content="in flight")
            assert retried["stored"] == "durable" and retried["outcome"] == "LANDED", retried
            assert retried["receipt"] is None and not retried["original_ack_received"]
            fresh = await mcp.call("context_remember", session_handle=handle, operation_id="op-2", content="next")
            assert fresh["stored"] == "durable"
            ids = operation_ids(story)
            assert ids.count("op-1") == 1 and ids.count("op-2") == 1, ids

    asyncio.run(asyncio.wait_for(run(), timeout=50))


def test_incomplete_control_lookup_never_yields_a_clean_open(tmp_path):
    async def run():
        chronicle, identity = unique("lookup"), unique("worker")
        async with Mcp(launcher(str(tmp_path), identity, chronicle)) as mcp:
            opened = await mcp.call("context_open", name="notes", create=True)
            await mcp.call("context_remember", session_handle=opened["session_handle"], operation_id="a",
                           content="kept")
            assert (await mcp.call("context_close", session_handle=opened["session_handle"]))["close_record"][
                "stored"] == "durable"
        # The newest aggregate is now older than the 1 s cut probe, so one read call cannot find it.
        time.sleep(1.2)
        bounded = launcher(str(tmp_path), identity, chronicle, "--checkpoint-lookup-max-read-calls", "1")
        async with Mcp(bounded) as mcp:
            status = await mcp.call("context_status")
            store = status["checkpoint_store"]
            assert store["state"] == "ready" and store["prior_state_unknown_below"], store
            reopened = await mcp.call("context_open", name="notes")
            assert reopened["state"] == "NEEDS_RECONCILE", reopened
            assert "prior state is unknown" in reopened["verdict"]
            handle = reopened["session_handle"]
            reconciled = await mcp.call("context_reconcile", session_handle=handle, operation_ids=[])
            assert reconciled["attempted"], reconciled
            stored = await mcp.call("context_remember", session_handle=handle, operation_id="b", content="after")
            assert stored["stored"] == "durable"
            # Every later aggregate keeps the unknown-prior provenance.
            await mcp.call("context_checkpoint")
            again = (await mcp.call("context_status"))["checkpoint_store"]
            assert again["prior_state_unknown_below"] == store["prior_state_unknown_below"]

    asyncio.run(asyncio.wait_for(run(), timeout=50))
