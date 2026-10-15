# capabilities/start_chronolog.py

from fastmcp.exceptions import ToolError

from chronomcp.utils import config
from chronomcp.utils.config import CL_ERR_CHRONICLE_EXISTS


async def start_chronolog(
    chronicle_name: str | None = None, story_name: str | None = None
) -> str:
    """Connect to ChronoLog and acquire a story handle for logging."""
    chronicle = chronicle_name or config.DEFAULT_CHRONICLE
    story = story_name or config.DEFAULT_STORY

    ret = config.get_client().Connect()
    if ret != 0:
        raise ToolError(f"Failed to connect to ChronoLog: {ret}")

    # a chronicle from an earlier session is reused
    ret = config.get_client().CreateChronicle(chronicle)
    if ret not in (0, CL_ERR_CHRONICLE_EXISTS):
        config.get_client().Disconnect()
        raise ToolError(f"Failed to create chronicle '{chronicle}': {ret}")

    ret, handle = config.get_client().AcquireStory(chronicle, story)
    if ret != 0:
        config.get_client().Disconnect()
        raise ToolError(
            f"Failed to acquire story '{story}' in chronicle '{chronicle}': {ret}"
        )

    config._active_chronicle = chronicle
    config._active_story = story
    config._story_handle = handle

    return f"ChronoLog session started: chronicle='{chronicle}', story='{story}'"
