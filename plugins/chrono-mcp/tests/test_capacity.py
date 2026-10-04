"""CAPACITY against a real Keeper (I13.16): this file's stack has no Grapher, so sealed chunks never settle, and its
Keeper runs with admission_cap_mb=1 and one-second chunks (plugins/chrono-mcp/CMakeLists.txt sets the environment)."""
import asyncio
import os

import pytest

from _mcp import Mcp, launcher, operation_ids, unique

pytestmark = pytest.mark.skipif(not os.getenv("CHRONOLOG_TEST_VISOR"), reason="real stack required")


def test_remember_at_the_keeper_cap_is_rejected_capacity_and_the_session_stays_ready(tmp_path):
    assert os.environ.get("CHRONOLOG_KEEPER_ADMISSION_CAP_MB") == "1", "the stack's Keeper must run with a 1 MiB cap"

    async def run():
        chronicle, identity = unique("capacity"), unique("planner")
        async with Mcp(launcher(str(tmp_path), identity, chronicle)) as mcp:
            opened = await mcp.call("context_open", name="notes", create=True)
            handle = opened["session_handle"]
            # Three DURABLE memories of 512 KiB each: 1.5 MiB that stays unsettled once its chunks seal.
            stored = []
            for n in range(3):
                filled = await mcp.call("context_remember", session_handle=handle, operation_id=f"fill-{n}",
                                        content="x" * (512 * 1024))
                assert filled["stored"] == "durable", {k: v for k, v in filled.items() if k != "content"}
                stored.append(f"fill-{n}")
            # The cap moves when the Keeper seals those chunks, which follows the chunk window and not this call, so
            # small memories are admitted until then. Each one is a new operation.
            refused = None
            for n in range(100):
                probe = await mcp.call("context_remember", session_handle=handle, operation_id=f"probe-{n}",
                                       content="small")
                if probe["stored"] == "rejected":
                    refused = probe
                    break
                assert probe["stored"] == "durable", probe
                stored.append(f"probe-{n}")
                await asyncio.sleep(0.1)
            assert refused is not None, "the Keeper never reached its admission cap"
            # The Keeper's verdict: a typed per-item rejection. The checkpoint store's own refusal carries no status.
            assert refused["outcome"] == "REJECTED", refused
            assert refused["status"]["code"] == "RESOURCE_EXHAUSTED", refused
            assert refused["status"]["rejection"] == "CAPACITY", refused
            assert refused["receipt"] is None, refused
            assert refused["state"] == "READY", refused
            assert refused["unresolved_operation_ids"] == [] and refused["blocking_operation_id"] is None, refused
            # The checkpoint store still had room for the operation: it was admitted there and dispatched.
            store = (await mcp.call("context_status"))["checkpoint_store"]
            assert store["encoded_bytes"] + store["reserve_bytes_per_session"] <= store["max_payload_bytes"], store
            # The same operation id keeps its definite outcome; a new one is refused by the Keeper again.
            again = await mcp.call("context_remember", session_handle=handle, operation_id=refused["operation_id"],
                                   content="small")
            assert again["stored"] == "rejected" and again["status"]["rejection"] == "CAPACITY", again
            fresh = await mcp.call("context_remember", session_handle=handle, operation_id="after-the-cap",
                                   content="small")
            assert fresh["stored"] == "rejected" and fresh["status"]["rejection"] == "CAPACITY", fresh
            assert fresh["state"] == "READY", fresh
            # No refused memory reached the story, and every admitted one is still read complete.
            assert operation_ids(int(opened["context"]["story_id"])) == stored

    asyncio.run(asyncio.wait_for(run(), timeout=50))
