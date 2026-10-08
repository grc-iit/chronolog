"""Tests for start_chronolog against a stand-in client (no deployment needed)."""

import pytest

try:
    from fastmcp.exceptions import ToolError

    from chronomcp.capabilities import start_handler
    from chronomcp.utils import config

    HAS_DEPENDENCIES = True
except ImportError:
    HAS_DEPENDENCIES = False

pytestmark = pytest.mark.skipif(
    not HAS_DEPENDENCIES,
    reason="ChronoLog Python client or fastmcp not available",
)


class FakeClient:
    """Takes the calls of the current Python client, with its arguments."""

    def __init__(self, create_ret=0, acquire_ret=0):
        self.create_ret = create_ret
        self.acquire_ret = acquire_ret
        self.calls = []

    def Connect(self):
        self.calls.append("Connect")
        return 0

    def Disconnect(self):
        self.calls.append("Disconnect")
        return 0

    def CreateChronicle(self, chronicle):
        self.calls.append(("CreateChronicle", chronicle))
        return self.create_ret

    def AcquireStory(self, chronicle, story):
        self.calls.append(("AcquireStory", chronicle, story))
        return self.acquire_ret, object()


@pytest.fixture
def fake(monkeypatch):
    def install(**kwargs):
        client = FakeClient(**kwargs)
        monkeypatch.setattr(config, "get_client", lambda: client)
        monkeypatch.setattr(config, "_story_handle", None)
        return client

    return install


@pytest.mark.asyncio
async def test_a_session_starts_with_the_clients_current_calls(fake):
    client = fake()
    result = await start_handler.start_chronolog("chron", "story")
    assert "ChronoLog session started" in result
    assert client.calls == ["Connect", ("CreateChronicle", "chron"), ("AcquireStory", "chron", "story")]
    assert (config._active_chronicle, config._active_story) == ("chron", "story")


@pytest.mark.asyncio
async def test_a_chronicle_from_an_earlier_session_is_reused(fake):
    fake(create_ret=start_handler.CL_ERR_CHRONICLE_EXISTS)
    assert "ChronoLog session started" in await start_handler.start_chronolog("chron", "story")


@pytest.mark.asyncio
async def test_a_story_that_cannot_be_acquired_ends_the_connection(fake):
    client = fake(acquire_ret=-4)
    with pytest.raises(ToolError):
        await start_handler.start_chronolog("chron", "story")
    assert client.calls[-1] == "Disconnect"
