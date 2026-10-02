import asyncio
import json
import os
import sys
import uuid

import chronolog as cl
import pytest

pytestmark = pytest.mark.skipif(not os.getenv("CHRONOLOG_TEST_VISOR"), reason="real stack required")


def test_mcp_stdio_session():
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client

    async def run():
        chronicle = f"mcp-{uuid.uuid4().hex}"
        client = cl.connect(os.environ["CHRONOLOG_TEST_VISOR"], os.getenv("CHRONOLOG_TEST_PLAYER"), timeout=3)
        parameters = StdioServerParameters(command=sys.executable, args=[
            "-m", "chronomcp.server", "--catalog", os.environ["CHRONOLOG_TEST_VISOR"],
            "--player", os.environ["CHRONOLOG_TEST_PLAYER"], "--chronicle", chronicle, "--timeout", "3"], env=dict(os.environ))
        story = None
        try:
            async with stdio_client(parameters) as (read, write):
                async with ClientSession(read, write) as session:
                    await session.initialize()

                    async def tool(name, args):
                        result = await session.call_tool(name, args)
                        assert not result.is_error, result
                        return json.loads(result.content[0].text)

                    created = await tool("create_story", {"story": "events"})
                    story = created["id"]
                    assert len(await tool("list_stories", {})) == 1
                    results = []
                    for index in range(3):
                        results.append(await tool("append", {"story": story, "content": str(index),
                                                                      "attributes": {"gen_ai.operation.name": "tool"}}))
                    assert all(r["acked"] and r["durability"] == "DURABLE" for r in results)
                    end = dict(results[-1]["hlc"])
                    end["logical"] += 1
                    for _ in range(40):
                        fetched = await tool("read", {"story": story, "end_hlc": end})
                        if fetched["completion"]["complete"]:
                            break
                        await asyncio.sleep(0.1)
                    assert fetched["completion"]["complete"]
                    assert [e["content"] for e in fetched["events"]] == ["0", "1", "2"]
                    assert all(e["attributes"]["requestId"] for e in fetched["events"])
                    assert fetched["events"][0]["attributes"]["gen_ai.operation.name"] == "tool"
                    pending = asyncio.create_task(tool("tail", {"story": story, "after": fetched["after"],
                                                                          "max_events": 1, "timeout_s": 3}))
                    await asyncio.sleep(0.2)
                    await tool("append", {"story": story, "content": "four"})
                    tailed = await pending
                    assert [e["content"] for e in tailed["events"]] == ["four"]
                    empty = await tool("tail", {"story": story, "after": tailed["after"],
                                                         "max_events": 1, "timeout_s": 0.2})
                    assert empty["events"] == [] and empty["timed_out"]
                    limited = await tool("read", {"story": story, "end_hlc": end, "limit": 1})
                    assert len(limited["events"]) == 1
                    assert limited["completion"] is None and limited["limited"]
            with client.acquire(story, "check-shutdown", timeout=3) as writer:
                result = writer.append(b"\xff", timeout=3)
            async with stdio_client(parameters) as (read, write):
                async with ClientSession(read, write) as session:
                    await session.initialize()
                    response = await session.call_tool("read", {"story": story,
                        "start_hlc": {"physical_ns": result.hlc.physical_ns, "logical": result.hlc.logical},
                        "end_hlc": {"physical_ns": result.hlc.physical_ns, "logical": result.hlc.logical + 1}})
                    binary = json.loads(response.content[0].text)["events"]
                    assert binary[0]["base64"] and binary[0]["content"] == "/w=="
        finally:
            if story is not None:
                client.destroy_story(story, timeout=3)
                client.destroy_chronicle(chronicle, timeout=3)

    asyncio.run(asyncio.wait_for(run(), timeout=40))


def test_conversation_workflow_resource_prompt_and_replay_reader():
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client

    async def run():
        chronicle = f"conversation-{uuid.uuid4().hex}"
        client = cl.connect(os.environ["CHRONOLOG_TEST_VISOR"], os.environ["CHRONOLOG_TEST_PLAYER"], timeout=3)
        parameters = StdioServerParameters(command=sys.executable, args=[
            "-m", "chronomcp.server", "--catalog", os.environ["CHRONOLOG_TEST_VISOR"],
            "--player", os.environ["CHRONOLOG_TEST_PLAYER"], "--chronicle", chronicle,
            "--identity", "conversation-test", "--timeout", "3"], env=dict(os.environ))
        story = None
        try:
            async with stdio_client(parameters) as (read, write):
                async with ClientSession(read, write) as session:
                    await session.initialize()
                    tools = await session.list_tools()
                    assert {t.name for t in tools.tools} == {
                        "start_chronolog", "record_interaction", "retrieve_interaction", "stop_chronolog",
                        "list_stories", "create_story", "append", "read", "tail"}
                    invalid = await session.call_tool("record_interaction", {"user_message": "before", "assistant_message": "start"})
                    assert invalid.is_error
                    status = await session.read_resource("chronolog://status")
                    assert json.loads(status.contents[0].text)["status"] == "idle"
                    prompt = await session.get_prompt("logging_workflow", {"time_range": "a requested HLC range"})
                    assert "Replay Completion" in prompt.messages[0].content.text
                    started = await session.call_tool("start_chronolog", {})
                    assert not started.is_error
                    story = json.loads(started.content[0].text)["story"]["id"]
                    again = await session.call_tool("start_chronolog", {})
                    assert json.loads(again.content[0].text)["story"]["id"] == story
                    recorded = await session.call_tool("record_interaction", {
                        "user_message": "question", "assistant_message": "answer", "trace_id": "11" * 16,
                        "span_id": "22" * 8, "attributes": {"gen_ai.tool.call.id": "call"}})
                    result = json.loads(recorded.content[0].text)
                    assert result["acked"]
                    end = dict(result["hlc"])
                    end["logical"] += 1
                    for _ in range(40):
                        fetched = await session.call_tool("retrieve_interaction", {"end_hlc": end})
                        assert not fetched.is_error
                        replay = json.loads(fetched.content[0].text)
                        if replay["completion"]["complete"]:
                            break
                        await asyncio.sleep(0.1)
                    assert replay["completion"]["complete"]
                    event = replay["events"][0]
                    assert json.loads(event["content"]) == {"user_message": "question", "assistant_message": "answer"}
                    assert event["attributes"]["gen_ai.conversation.id"] == f"{chronicle}/conversation"
                    assert event["attributes"]["gen_ai.tool.call.id"] == "call"
                    assert event["attributes"]["requestId"]
                    assert event["trace_id"] == "11" * 16 and event["span_id"] == "22" * 8
                    stopped = await session.call_tool("stop_chronolog", {})
                    assert json.loads(stopped.content[0].text)["status"] == "stopped"
                    status = await session.read_resource("chronolog://status")
                    assert json.loads(status.contents[0].text)["writer_id"] is None
                    assert (await session.call_tool("record_interaction", {"user_message": "after", "assistant_message": "stop"})).is_error
            with client.acquire(story, "conversation-test", timeout=3) as writer:
                assert writer.incarnation == 2
            process = await asyncio.create_subprocess_exec(sys.executable, "-m", "chronomcp.reader", str(story),
                "--catalog", os.environ["CHRONOLOG_TEST_VISOR"], "--player", os.environ["CHRONOLOG_TEST_PLAYER"],
                "--end-hlc", json.dumps(end), "--timeout", "3", stdout=asyncio.subprocess.PIPE)
            try:
                stdout, _ = await asyncio.wait_for(process.communicate(), timeout=5)
                assert process.returncode == 0
                assert json.loads(stdout)["events"][0]["id"] == event["id"]
            finally:
                if process.returncode is None:
                    process.kill()
                    await process.wait()
        finally:
            if story is not None:
                client.destroy_story(story, timeout=3)
                client.destroy_chronicle(chronicle, timeout=3)

    asyncio.run(asyncio.wait_for(run(), timeout=40))


def test_http_sessions_share_one_process_writer():
    import socket
    from mcp import ClientSession
    from mcp.client.streamable_http import streamable_http_client

    async def run():
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        chronicle = f"http-{uuid.uuid4().hex}"
        client = cl.connect(os.environ["CHRONOLOG_TEST_VISOR"], os.environ["CHRONOLOG_TEST_PLAYER"], timeout=3)
        process = await asyncio.create_subprocess_exec(sys.executable, "-m", "chronomcp.server", "--http",
            "--port", str(port), "--catalog", os.environ["CHRONOLOG_TEST_VISOR"],
            "--player", os.environ["CHRONOLOG_TEST_PLAYER"], "--chronicle", chronicle, "--timeout", "3",
            stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.DEVNULL)
        story = None
        try:
            for _ in range(100):
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                        break
                except OSError:
                    assert process.returncode is None
                    await asyncio.sleep(0.05)
            else:
                pytest.fail("HTTP server did not start")
            async with streamable_http_client(f"http://127.0.0.1:{port}/mcp") as (read1, write1):
                async with ClientSession(read1, write1) as first:
                    await first.initialize()
                    async with streamable_http_client(f"http://127.0.0.1:{port}/mcp") as (read2, write2):
                        async with ClientSession(read2, write2) as second:
                            await second.initialize()
                            created = await first.call_tool("create_story", {"story": "shared"})
                            assert not created.is_error
                            story = json.loads(created.content[0].text)["id"]
                            one = await first.call_tool("append", {"story": story, "content": "first"})
                            two = await second.call_tool("append", {"story": story, "content": "second"})
                            assert not one.is_error and not two.is_error
                            id1, id2 = [json.loads(r.content[0].text)["event_id"] for r in (one, two)]
                            assert id1["writer_id"] == id2["writer_id"]
                            assert id2["sequence"] == id1["sequence"] + 1
                            assert not (await second.call_tool("stop_chronolog", {})).is_error
        finally:
            process.terminate()
            try:
                await asyncio.wait_for(process.wait(), timeout=5)
            except asyncio.TimeoutError:
                process.kill()
                await process.wait()
            if story is not None:
                client.destroy_story(story, timeout=3)
                client.destroy_chronicle(chronicle, timeout=3)

    asyncio.run(asyncio.wait_for(run(), timeout=40))
