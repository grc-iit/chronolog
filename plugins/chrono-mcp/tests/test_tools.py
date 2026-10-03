import asyncio
import base64
import os

import pytest

from _mcp import Mcp, launcher, operation_ids, unique, until

pytestmark = pytest.mark.skipif(not os.getenv("CHRONOLOG_TEST_VISOR"), reason="real stack required")

TOOLS = {"context_open", "context_remember", "context_recall", "context_latest", "context_follow",
         "context_reconcile", "context_checkpoint", "context_close", "context_list", "context_status"}


def contents(result):
    return [e["content"]["data"] for e in result["events"]]


def test_ten_tools_round_trip_and_clean_close_reopens_without_recovery(tmp_path):
    async def run():
        chronicle, identity = unique("tools"), unique("planner")
        args = launcher(str(tmp_path), identity, chronicle)
        async with Mcp(args) as mcp:
            listed = {t.name: " ".join(t.description.split()) for t in (await mcp.session.list_tools()).tools}
            assert set(listed) == TOOLS
            assert "reuse operation_id when retrying after an error or timeout" in listed["context_remember"].lower()
            assert ("pass every operation_id for which you have not seen an outcome; omitted ids may duplicate"
                    in listed["context_reconcile"].lower())
            missing = await mcp.call("context_open", name="notes")
            assert missing["session_handle"] is None and missing["status"]["code"] == "NOT_FOUND"
            opened = await mcp.call("context_open", name="notes", create=True)
            assert list(opened)[:4] == ["verdict", "answer_complete", "has_more", "next_cursor"]
            assert opened["state"] == "READY" and opened["identity"]["label"] == "self"
            handle, story = opened["session_handle"], int(opened["context"]["story_id"])
            assert (await mcp.call("context_open", name="notes"))["session_handle"] == handle
            listed = await mcp.call("context_list")
            assert [c["name"] for c in listed["contexts"]] == ["notes"]

            first = await mcp.call("context_remember", session_handle=handle, operation_id="op-1", content="alpha")
            assert first["stored"] == "durable" and first["answer_complete"]
            binary = await mcp.call("context_remember", session_handle=handle, operation_id="op-2",
                                    content={"encoding": "base64", "data": base64.b64encode(b"\xff\x00").decode()})
            assert binary["stored"] == "durable"
            third = await mcp.call("context_remember", session_handle=handle, operation_id="op-3", content="gamma",
                                   attributes={"gen_ai.tool.call.id": "call-7"})
            again = await mcp.call("context_remember", session_handle=handle, operation_id="op-1", content="alpha")
            assert again["receipt"] == first["receipt"]
            conflict = await mcp.call("context_remember", session_handle=handle, operation_id="op-1", content="other")
            assert conflict["stored"] == "rejected" and conflict["status"]["code"] == "FAILED_PRECONDITION"
            assert operation_ids(story) == ["op-1", "op-2", "op-3"]

            recalled = await until(lambda: mcp.call("context_recall", session_handle=handle),
                                   lambda r: r["answer_complete"])
            assert contents(recalled) == ["alpha", "/wA=", "gamma"]
            assert recalled["events"][1]["content"]["encoding"] == "base64"
            assert "attributes" in recalled["omitted"] and "attributes" not in recalled["events"][2]
            assert recalled["events"][0]["id"]["sequence"] == "1" and recalled["follow_token"]
            full = await mcp.call("context_recall", session_handle=handle, view="full")
            assert full["events"][-1]["attributes"]["gen_ai.tool.call.id"] == "call-7"
            assert full["events"][-1]["attributes"]["chronolog.operation.id"] == "op-3"
            before = await mcp.call("context_recall", session_handle=handle, end=recalled["events"][2]["at"])
            assert contents(before) == ["alpha", "/wA="]
            paged, page = [], await mcp.call("context_recall", session_handle=handle, max_events=1)
            while True:
                paged += contents(page)
                if page["next_cursor"] is None:
                    break
                page = await mcp.call("context_recall", session_handle=handle, cursor=page["next_cursor"],
                                      max_events=1)
            assert paged == ["alpha", "/wA=", "gamma"] and page["answer_complete"]
            tail = await mcp.call("context_recall", session_handle=handle, start=recalled["events"][1]["at"])
            assert contents(tail) == ["/wA=", "gamma"]
            latest = await until(lambda: mcp.call("context_latest", session_handle=handle, n=2),
                                 lambda r: r["selection_complete"])
            assert contents(latest) == ["/wA=", "gamma"] and latest["answer_complete"] and latest["as_of_token"]

            followed = await mcp.call("context_follow", subscriptions=[{"session_handle": handle,
                                                                         "from": "beginning"}], timeout_s=5)
            got, token = contents(followed["pages"][0]), followed["pages"][0]["next_follow_token"]
            while len(got) < 3:
                more = await mcp.call("context_follow", subscriptions=[{"session_handle": handle, "from": token}])
                got, token = got + contents(more["pages"][0]), more["pages"][0]["next_follow_token"]
            assert got == ["alpha", "/wA=", "gamma"] and not followed["answer_complete"]
            waiting = asyncio.create_task(mcp.call("context_follow", subscriptions=[
                {"session_handle": handle, "from": token}], timeout_s=10))
            await asyncio.sleep(0.3)
            await mcp.call("context_remember", session_handle=handle, operation_id="op-4", content="delta")
            woke = await waiting
            assert contents(woke["pages"][0]) == ["delta"]
            token = woke["pages"][0]["next_follow_token"]
            idle = await mcp.call("context_follow", subscriptions=[{"session_handle": handle, "from": token}],
                                  timeout_s=0.5)
            assert idle["idle"] and idle["pages"][0]["next_follow_token"] == token

            saved = await mcp.call("context_checkpoint", session_handle=handle, processed=[token])
            assert saved["stored"] == "durable" and saved["checkpoint_id"] and saved["acknowledged"] == [token]
            status = await mcp.call("context_status")
            assert status["checkpoint_store"]["state"] == "ready"
            assert status["sessions"][0]["writer"] == opened["writer"]
            reader = await mcp.call("context_open", ref_token=opened["ref_token"], agent="reviewer",
                                    access="read_only")
            assert reader["writer"] is None and reader["state"] == "READY"
            refused = await mcp.raw("context_remember", session_handle=reader["session_handle"], operation_id="r",
                                    content="no")
            assert refused.is_error
            closed = await mcp.call("context_close", session_handle=handle)
            assert closed["release_committed"] and closed["close_record"]["stored"] == "durable"
            assert (await mcp.raw("context_remember", session_handle=handle, operation_id="op-5",
                                  content="late")).is_error
        # A durably recorded clean close lets the next launch acquire the next incarnation directly, and a named
        # checkpoint restores the processed follow position.
        async with Mcp(args) as mcp:
            reopened = await mcp.call("context_open", name="notes", resume=True)
            assert reopened["state"] == "READY", reopened
            assert int(reopened["writer"]["incarnation"]) == int(opened["writer"]["incarnation"]) + 1
            assert reopened["writer"]["writer_id"] == opened["writer"]["writer_id"]
            assert reopened["processed_after"] == token
            named = await mcp.call("context_open", name="notes", access="read_only",
                                   checkpoint_id=saved["checkpoint_id"])
            assert named["state"] == "READY" and named["processed_after"] == token and named["writer"] is None
            stranger = await mcp.raw("context_open", name="notes", agent="other", checkpoint_id=saved["checkpoint_id"])
            assert stranger.is_error

    asyncio.run(asyncio.wait_for(run(), timeout=50))
