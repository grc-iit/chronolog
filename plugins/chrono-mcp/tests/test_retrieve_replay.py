"""Tests for retrieve_interaction against a stand-in client (no deployment needed)."""

import os

import pytest

try:
    from fastmcp.exceptions import ToolError

    from chronomcp.capabilities import retrieve_handler
    from chronomcp.utils import config

    HAS_DEPENDENCIES = True
except ImportError:
    HAS_DEPENDENCIES = False

pytestmark = pytest.mark.skipif(
    not HAS_DEPENDENCIES,
    reason="ChronoLog Python client or fastmcp not available",
)


class FakeEvent:
    def __init__(self, record):
        self._record = record

    def log_record(self):
        return self._record


class FakeReader:
    """Records the calls retrieve makes and answers as told."""

    def __init__(self, records, replay_ret=0, acquire_ret=0, create_ret=0, release_ret=0):
        self.release_ret = release_ret
        self.records = records
        self.replay_ret = replay_ret
        self.acquire_ret = acquire_ret
        self.create_ret = create_ret
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

    def ReleaseStory(self, chronicle, story):
        self.calls.append(("ReleaseStory", chronicle, story))
        return self.release_ret

    def ReplayStory(self, chronicle, story, start, end, events):
        self.calls.append(("ReplayStory", chronicle, story, start, end))
        events.extend(FakeEvent(r) for r in self.records)
        return self.replay_ret


@pytest.fixture
def fake(monkeypatch, tmp_path):
    def install(records, session=None, **kwargs):
        """session: the (chronicle, story) a start_chronolog session holds, or None"""
        reader = FakeReader(records, **kwargs)
        # replaces the factory, so no real client is made
        monkeypatch.setattr(config, "get_client", lambda: reader)
        monkeypatch.setattr(config, "_story_handle", object() if session else None)
        monkeypatch.setattr(config, "_active_chronicle", session[0] if session else None)
        monkeypatch.setattr(config, "_active_story", session[1] if session else None)
        # an EventList is a bound std::vector; a list stands in for it
        monkeypatch.setattr(retrieve_handler.py_chronolog_client, "EventList", list)
        monkeypatch.chdir(tmp_path)
        return reader

    return install


@pytest.mark.asyncio
async def test_records_are_written_to_a_file(fake):
    reader = fake(["q1 a1", "q2 a2"])
    result = await retrieve_handler.retrieve_interaction("chron", "story", "1000", "2000")
    with open(result) as f:
        assert f.read() == "q1 a1\nq2 a2"
    assert ("ReplayStory", "chron", "story", 1000, 2000) in reader.calls


@pytest.mark.asyncio
async def test_the_reader_connects_creates_acquires_and_lets_go(fake):
    reader = fake([])
    assert await retrieve_handler.retrieve_interaction("chron", "story") == "No records found."
    assert reader.calls[:3] == ["Connect", ("CreateChronicle", "chron"), ("AcquireStory", "chron", "story")]
    assert reader.calls[-2:] == [("ReleaseStory", "chron", "story"), "Disconnect"]


@pytest.mark.asyncio
async def test_a_session_on_the_same_story_is_used_as_it_is(fake):
    reader = fake(["r"], session=("chron", "story"))
    await retrieve_handler.retrieve_interaction("chron", "story")
    assert [c for c in reader.calls if not isinstance(c, tuple) or c[0] != "ReplayStory"] == []


@pytest.mark.asyncio
async def test_a_session_on_another_story_acquires_this_one_on_the_same_connection(fake):
    reader = fake(["r"], session=("chron", "other"))
    await retrieve_handler.retrieve_interaction("chron", "story")
    assert "Connect" not in reader.calls and "Disconnect" not in reader.calls
    assert ("AcquireStory", "chron", "story") in reader.calls
    assert ("ReleaseStory", "chron", "story") in reader.calls


# The visor forgets chronicles when it restarts; their archived events must
# still come back.
@pytest.mark.asyncio
async def test_a_chronicle_the_visor_already_has_is_used_as_it_is(fake):
    reader = fake(["r"], create_ret=retrieve_handler.CL_ERR_CHRONICLE_EXISTS)
    assert await retrieve_handler.retrieve_interaction("chron", "story") != "No records found."
    assert ("AcquireStory", "chron", "story") in reader.calls


@pytest.mark.asyncio
async def test_a_chronicle_that_cannot_be_created_is_an_error(fake):
    reader = fake([], create_ret=-8)
    with pytest.raises(ToolError):
        await retrieve_handler.retrieve_interaction("chron", "story")
    assert not any(isinstance(c, tuple) and c[0] == "AcquireStory" for c in reader.calls)
    assert reader.calls[-1] == "Disconnect"


@pytest.mark.asyncio
async def test_a_partial_replay_returns_what_came_back_and_says_so(fake):
    fake(["r"], replay_ret=retrieve_handler.CL_ERR_PARTIAL_RESULT)
    result = await retrieve_handler.retrieve_interaction("chron", "story")
    assert "partial" in result
    assert os.path.exists(result.split(" ")[0])


@pytest.mark.asyncio
async def test_a_partial_replay_with_nothing_is_not_called_empty(fake):
    fake([], replay_ret=retrieve_handler.CL_ERR_PARTIAL_RESULT)
    result = await retrieve_handler.retrieve_interaction("chron", "story")
    assert result != "No records found."
    assert "missing" in result


@pytest.mark.asyncio
async def test_a_failed_replay_is_an_error_and_still_lets_go(fake):
    reader = fake([], replay_ret=-12)
    with pytest.raises(ToolError):
        await retrieve_handler.retrieve_interaction("chron", "story")
    assert reader.calls[-2:] == [("ReleaseStory", "chron", "story"), "Disconnect"]


@pytest.mark.asyncio
async def test_a_story_that_could_not_be_released_is_reported(fake):
    fake([], release_ret=-5)
    result = await retrieve_handler.retrieve_interaction("chron", "story")
    assert result.startswith("No records found.")
    assert "could not release" in result
