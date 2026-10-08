"""Shared fixtures for the ChronoLog MCP tests."""

import pytest

try:
    from chronomcp.utils import config
except Exception:  # the Python client or fastmcp missing, or failing to load
    config = None


@pytest.fixture(autouse=True)
def no_session_left_open():
    """End any session a test left open, so the next test starts without one."""
    yield
    if config is None:
        return
    if config._story_handle is not None:
        try:
            client = config.get_client()
            client.ReleaseStory(config._active_chronicle, config._active_story)
            client.Disconnect()
        except Exception:
            pass
    config._story_handle = None
    config._active_chronicle = None
    config._active_story = None
