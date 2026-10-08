# capabilities/retrieve_interaction.py

import time
from datetime import datetime

from fastmcp.exceptions import ToolError

import py_chronolog_client
from chronomcp.utils import config, helpers

from chronomcp.utils.config import CL_ERR_CHRONICLE_EXISTS, CL_ERR_PARTIAL_RESULT


async def retrieve_interaction(
    chronicle_name: str | None = None,
    story_name: str | None = None,
    start_time: str | None = None,
    end_time: str | None = None,
) -> str:
    """Replay a story through ChronoPlayer and write its records to a file.

    The replay covers both the HDF5 archive and the events ChronoKeeper still
    holds, so it does not depend on the archive's file layout.
    """
    chronicle = chronicle_name or config.DEFAULT_CHRONICLE
    story = story_name or config.DEFAULT_STORY
    start_ns = int(helpers.parse_time_arg(start_time, is_end=False)) if start_time else 1
    end_ns = (
        int(helpers.parse_time_arg(end_time, is_end=True))
        if end_time
        else time.time_ns() + 60 * 1_000_000_000
    )

    # A replay needs a connected client that has acquired the story. Outside a
    # session, connect for it; unless the session holds this story, acquire it.
    # The visor keeps chronicles and stories in memory only, so after it
    # restarts it knows none of the archived ones: the chronicle is created
    # first, as the C++ reader example does, and acquiring creates the story. A
    # name nothing was ever written to therefore comes out as an empty story.
    client = config.get_client()
    connected_here = config._story_handle is None
    acquired_here = connected_here or (config._active_chronicle, config._active_story) != (chronicle, story)
    release_ret = 0
    if connected_here:
        ret = client.Connect()
        if ret != 0:
            raise ToolError(f"Failed to connect to ChronoLog: {ret}")
    try:
        if acquired_here:
            ret = client.CreateChronicle(chronicle)
            if ret not in (0, CL_ERR_CHRONICLE_EXISTS):
                raise ToolError(f"Failed to create chronicle '{chronicle}': {ret}")
            ret, _ = client.AcquireStory(chronicle, story)
            if ret != 0:
                raise ToolError(f"Failed to acquire story '{story}' of chronicle '{chronicle}': {ret}")
        try:
            events = py_chronolog_client.EventList()
            ret = client.ReplayStory(chronicle, story, start_ns, end_ns, events)
        finally:
            if acquired_here:
                release_ret = client.ReleaseStory(chronicle, story)
    finally:
        if connected_here:
            client.Disconnect()

    if ret != 0 and ret != CL_ERR_PARTIAL_RESULT:
        raise ToolError(f"Failed to replay story '{story}' of chronicle '{chronicle}': {ret}")
    # a story left acquired blocks a later start or destroy of it: say so
    note = f" (could not release story '{story}': {release_ret})" if release_ret != 0 else ""
    records = [event.log_record() for event in events]
    if not records:
        if ret == CL_ERR_PARTIAL_RESULT:
            return "No records found, but ChronoPlayer reported some events missing; retrying may return them." + note
        return "No records found." + note

    ts = datetime.now().strftime("%Y%m%d%H%M%S")
    filename = f"records_{chronicle}_{story}_{ts}.txt"
    with open(filename, "w") as f:
        f.write("\n".join(records))
    if ret == CL_ERR_PARTIAL_RESULT:
        return f"{filename} (partial: ChronoPlayer reported some events missing; retrying may return more)" + note
    return filename + note
