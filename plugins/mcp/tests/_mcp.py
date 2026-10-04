"""One chronolog-mcp stdio process per `async with`, driven through the MCP client; its pid allows a hard kill."""
import asyncio
from contextlib import AsyncExitStack
import json
import os
import signal
import sys
import tempfile
import time
import uuid

import chronolog as cl

LAUNCH = ("import os, sys; open(sys.argv[1], 'w').write(str(os.getpid())); "
          "os.execv(sys.executable, [sys.executable, '-m', 'chronomcp.server', *sys.argv[2:]])")
VISOR = os.environ.get("CHRONOLOG_TEST_VISOR")
PLAYER = os.environ.get("CHRONOLOG_TEST_PLAYER")


def unique(name):
    return f"mcp-{name}-{uuid.uuid4().hex[:12]}"


def launcher(tmp, identity, chronicle, *extra, host="host-a", lock="locks"):
    return ["--catalog", VISOR, "--player", PLAYER, "--chronicle", chronicle, "--identity", identity,
            "--state-chronicle", chronicle + "-state", "--host-id", host, "--lock-dir", os.path.join(tmp, lock),
            "--timeout", "5", *extra]


class Mcp:
    def __init__(self, args):
        self.args = args

    async def __aenter__(self):
        from mcp import ClientSession, StdioServerParameters
        from mcp.client.stdio import stdio_client
        handle, self.pidfile = tempfile.mkstemp()
        os.close(handle)
        parameters = StdioServerParameters(command=sys.executable, args=["-c", LAUNCH, self.pidfile, *self.args],
                                           env=dict(os.environ))
        self.stack = AsyncExitStack()
        read, write = await self.stack.enter_async_context(stdio_client(parameters))
        self.session = await self.stack.enter_async_context(ClientSession(read, write))
        await self.session.initialize()
        with open(self.pidfile) as f:
            self.pid = int(f.read())
        return self

    async def raw(self, tool, /, **arguments):
        return await self.session.call_tool(tool, arguments)

    async def call(self, tool, /, **arguments):
        result = await self.raw(tool, **arguments)
        assert not result.is_error, result
        return json.loads(result.content[0].text)

    def kill(self):
        os.kill(self.pid, signal.SIGKILL)

    async def __aexit__(self, *exc):
        try:
            await asyncio.wait_for(self.stack.aclose(), 10)
        except Exception:  # noqa: BLE001 a killed server breaks its pipes
            if exc[0] is None and not self.killed():
                raise
        finally:
            os.unlink(self.pidfile)
        return False

    def killed(self):
        try:
            os.kill(self.pid, 0)
            return False
        except ProcessLookupError:
            return True


async def until(operation, done, what="answer"):
    """A verified cut may wait on a lagging writer; the native answer says when it is complete."""
    for _ in range(50):
        last = await operation()
        if done(last):
            return last
        await asyncio.sleep(0.1)
    raise AssertionError(f"{what} never completed: {last}")


def raw_client():
    return cl.connect(VISOR, PLAYER, timeout=5)


def operation_ids(story_id):
    """Operation ids of every event in the story, read straight from Replay to a complete Completion."""
    client = raw_client()
    for _ in range(50):
        with client.read(story_id, None, cl.Hlc(time.time_ns()), timeout=5) as reader:
            ids = [e.envelope.attributes.get("chronolog.operation.id") for e in reader]
            if reader.completion is not None and reader.completion.complete:
                return ids
        time.sleep(0.1)
    raise AssertionError("raw Replay never completed")
