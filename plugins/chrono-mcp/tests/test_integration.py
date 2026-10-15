"""Integration tests for Chronolog MCP Server."""

import asyncio
import os

import pytest
import time
import random

try:
    from fastmcp.exceptions import ToolError

    from chronomcp.capabilities import (
        record_handler,
        retrieve_handler,
        start_handler,
        stop_handler,
    )

    HAS_DEPENDENCIES = True
except ImportError:
    HAS_DEPENDENCIES = False

from .test_utils import are_chronolog_processes_running

pytestmark = pytest.mark.skipif(
    not HAS_DEPENDENCIES,
    reason="ChronoLog system dependencies not available",
)


class TestIntegration:
    """Integration tests for the full Chronolog MCP stack"""

    @pytest.mark.asyncio
    async def test_basic_workflow(self):
        """Test basic workflow: start -> record -> retrieve -> stop"""
        if not are_chronolog_processes_running():
            pytest.skip("ChronoLog processes are not running")

        chronicle_name = (
            f"test_chronicle_{int(time.time())}_{random.randint(1000, 9999)}"
        )
        story_name = f"test_story_{int(time.time())}_{random.randint(1000, 9999)}"

        # Start session
        start_result = await start_handler.start_chronolog(chronicle_name, story_name)
        assert isinstance(start_result, str)
        assert "ChronoLog session started" in start_result

        # Record interaction
        record_result = await record_handler.record_interaction("Hello", "Hi there!")
        assert isinstance(record_result, str)
        assert record_result == "Interaction recorded to ChronoLog"

        # Retrieve interactions. A replay sees an event once its keeper has
        # sealed the chunk holding it (about 25 s with the template's 10 s
        # chunks and 15 s acceptance window), so ask until it shows up.
        deadline = time.time() + 60
        while True:
            try:
                retrieve_result = await retrieve_handler.retrieve_interaction(
                    chronicle_name, story_name
                )
            except ToolError as error:  # a replay that timed out, say: ask again
                retrieve_result = str(error)
            # a records file means events came back; anything else is a message
            records_file = retrieve_result.split(" ")[0]
            if os.path.isfile(records_file) or time.time() > deadline:
                break
            await asyncio.sleep(5)
        assert os.path.isfile(records_file), retrieve_result
        os.remove(records_file)

        # Stop session
        stop_result = await stop_handler.stop_chronolog()
        assert isinstance(stop_result, str)
        assert "ChronoLog session stopped" in stop_result
