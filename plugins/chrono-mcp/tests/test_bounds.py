import asyncio
import os
import subprocess
import sys

import chronolog as cl
import pytest

from _mcp import Mcp, launcher, operation_ids, unique

pytestmark = pytest.mark.skipif(not os.getenv("CHRONOLOG_TEST_VISOR"), reason="real stack required")


def test_checkpoint_payload_bound_refuses_before_dispatch(tmp_path):
    async def run():
        chronicle, identity = unique("bound"), unique("planner")
        async with Mcp(launcher(str(tmp_path), identity, chronicle)) as mcp:
            opened = await mcp.call("context_open", name="notes", create=True)
            store = (await mcp.call("context_status"))["checkpoint_store"]
            await mcp.call("context_close", session_handle=opened["session_handle"])
        encoded, reserve = store["encoded_bytes"], store["reserve_bytes_per_session"]
        # Room for the session, its lifecycle reserve and one short id, but not one maximum-length id whose 128
        # control bytes each expand to a six-byte JSON escape.
        cap = encoded + 2 * reserve - 400
        async with Mcp(launcher(str(tmp_path), identity, chronicle, "--max-checkpoint-payload-bytes",
                                str(cap))) as mcp:
            handle = (await mcp.call("context_open", name="notes"))["session_handle"]
            short = await mcp.call("context_remember", session_handle=handle, operation_id="s", content="fits")
            assert short["stored"] == "durable", short
            refused = await mcp.call("context_remember", session_handle=handle, operation_id="\x01" * 128,
                                     content="too large to track")
            assert refused["stored"] == "rejected" and "RESOURCE_EXHAUSTED" in refused["verdict"], refused
            assert operation_ids(int(opened["context"]["story_id"])) == ["s"]
        over = subprocess.run([sys.executable, "-m", "chronomcp.server", *launcher(
            str(tmp_path), identity, chronicle, "--max-checkpoint-payload-bytes", "2048",
            "--keeper-payload-max-bytes", "1024")], capture_output=True, text=True, timeout=30, stdin=subprocess.DEVNULL)
        assert over.returncode != 0 and "payload_max_bytes" in over.stderr

    asyncio.run(asyncio.wait_for(run(), timeout=50))


def test_follow_tokens_resume_across_a_restart(tmp_path):
    async def run():
        chronicle, identity = unique("follow"), unique("planner")
        args = launcher(str(tmp_path), identity, chronicle)
        async with Mcp(args) as mcp:
            handle = (await mcp.call("context_open", name="feed", create=True))["session_handle"]
            await mcp.call("context_remember", session_handle=handle, operation_id="e1", content="one")
            followed = await mcp.call("context_follow", subscriptions=[{"session_handle": handle,
                                                                         "from": "beginning"}])
            assert [e["content"]["data"] for e in followed["pages"][0]["events"]] == ["one"]
            token = followed["pages"][0]["next_follow_token"]
            now = await mcp.call("context_follow", subscriptions=[{"session_handle": handle, "from": "now"}],
                                 timeout_s=0.5)
            assert now["idle"] and now["pages"][0]["starting_cut"] and now["pages"][0]["next_follow_token"]
            await mcp.call("context_remember", session_handle=handle, operation_id="e2", content="two")
        async with Mcp(args) as mcp:
            reader = (await mcp.call("context_open", name="feed", agent="watcher", access="read_only"))[
                "session_handle"]
            resumed = await mcp.call("context_follow", subscriptions=[{"session_handle": reader, "from": token}])
            assert [e["content"]["data"] for e in resumed["pages"][0]["events"]] == ["two"]
            assert not resumed["answer_complete"] and resumed["next_cursor"] is None
            # Server stop closed the writable session durably, so the next writable open needs no recovery.
            writer = await mcp.call("context_open", name="feed")
            assert writer["state"] == "READY", writer

    asyncio.run(asyncio.wait_for(run(), timeout=50))


def test_idle_writable_sessions_close_and_record_their_close(tmp_path):
    async def run():
        chronicle, identity = unique("idle"), unique("planner")
        async with Mcp(launcher(str(tmp_path), identity, chronicle, "--idle-close-s", "1")) as mcp:
            handle = (await mcp.call("context_open", name="quiet", create=True))["session_handle"]
            await mcp.call("context_remember", session_handle=handle, operation_id="q1", content="one")
            # The idle timer checks every 0.5 s: the session closes after 1 s idle, the store 1 s after its close.
            await asyncio.sleep(3.5)
            late = await mcp.raw("context_remember", session_handle=handle, operation_id="q2", content="two")
            assert late.is_error and "idle" in late.content[0].text
            assert (await mcp.call("context_status"))["checkpoint_store"]["state"] == "closed"
            again = await mcp.call("context_open", name="quiet")
            assert again["state"] == "READY", again
            stored = await mcp.call("context_remember", session_handle=again["session_handle"], operation_id="q2",
                                    content="two")
            assert stored["stored"] == "durable"

    asyncio.run(asyncio.wait_for(run(), timeout=50))


def test_status_names_capacity_and_keeps_unknown_rejections():
    from chronomcp.server import _status
    assert _status(cl._status(8, "admission capacity", 13))["rejection"] == "CAPACITY"
    assert _status(cl._status(9, "new rejection", 14))["rejection"] == "UNKNOWN_14"
