"""A server that exits cleanly closes its sessions; a killed one leaves NeedsReconcile for the next open."""
import asyncio
import json
import os
import select
import signal
import subprocess
import sys

import pytest
from mcp_types.version import LATEST_PROTOCOL_VERSION

from _mcp import Mcp, launcher, unique

pytestmark = pytest.mark.skipif(not os.getenv("CHRONOLOG_TEST_VISOR"), reason="real stack required")


class Raw:
    """chronolog-mcp over plain pipes, so the test alone decides how its stdin ends."""

    def __init__(self, args, log):
        self.proc = subprocess.Popen([sys.executable, "-m", "chronomcp.server", *args], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=log, env=dict(os.environ))
        self.next_id = 0
        self.request("initialize", protocolVersion=LATEST_PROTOCOL_VERSION, capabilities={},
                     clientInfo={"name": "test", "version": "0"})
        self.send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def send(self, message):
        self.proc.stdin.write((json.dumps(message) + "\n").encode())
        self.proc.stdin.flush()

    def request(self, method, **params):
        self.next_id += 1
        self.send({"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params})
        for _ in range(100):
            ready, _, _ = select.select([self.proc.stdout], [], [], 15)
            assert ready, f"no answer to {method}"
            reply = json.loads(self.proc.stdout.readline())
            if reply.get("id") == self.next_id:
                assert "error" not in reply, reply
                return reply["result"]
        raise AssertionError(f"no answer to {method}")

    def call(self, tool, **arguments):
        result = self.request("tools/call", name=tool, arguments=arguments)
        assert not result.get("isError"), result
        return json.loads(result["content"][0]["text"])


def remembered(args, log):
    raw = Raw(args, log)
    handle = raw.call("context_open", name="notes", create=True)["session_handle"]
    stored = raw.call("context_remember", session_handle=handle, operation_id="one", content="kept")
    assert stored["stored"] == "durable", stored
    return raw


async def reopen(args):
    async with Mcp(args) as mcp:
        return await mcp.call("context_open", name="notes")


@pytest.mark.parametrize("end", ["eof", "sigterm", "sigint"])
def test_clean_exit_closes_sessions_and_the_next_open_is_clean(tmp_path, end):
    args = launcher(str(tmp_path), unique("owner"), unique("shutdown"))
    with open(tmp_path / "server.log", "wb") as log:
        raw = remembered(args, log)
        if end == "eof":
            raw.proc.stdin.close()
        else:
            raw.proc.send_signal(signal.SIGTERM if end == "sigterm" else signal.SIGINT)
        assert raw.proc.wait(timeout=30) == 0, (tmp_path / "server.log").read_text()
    opened = asyncio.run(asyncio.wait_for(reopen(args), 30))
    assert opened["state"] == "READY" and opened["verdict"] == "open", opened


def test_killed_server_leaves_needs_reconcile(tmp_path):
    args = launcher(str(tmp_path), unique("owner"), unique("killed"))
    with open(tmp_path / "server.log", "wb") as log:
        raw = remembered(args, log)
        raw.proc.kill()
        assert raw.proc.wait(timeout=30) == -signal.SIGKILL
    opened = asyncio.run(asyncio.wait_for(reopen(args), 30))
    assert opened["state"] == "NEEDS_RECONCILE" and not opened["takeover_required"], opened
    assert "context_reconcile" in opened["verdict"], opened
