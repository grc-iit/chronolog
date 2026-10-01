import asyncio
import json
import os
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


async def main():
    params = StdioServerParameters(command="chronolog-mcp", args=[], env={
        "CHRONOLOG_CATALOG": os.getenv("CHRONOLOG_CATALOG", "127.0.0.1:50051"),
        "CHRONOLOG_PLAYER": os.getenv("CHRONOLOG_PLAYER", "127.0.0.1:50054")})
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            await session.call_tool("start_chronolog", {"chronicle_name": "example", "story_name": "conversation"})
            await session.call_tool("record_interaction", {"user_message": "What happened?", "assistant_message": "The job finished."})
            result = await session.call_tool("retrieve_interaction", {})
            print(json.dumps(json.loads(result.content[0].text), indent=2))
            await session.call_tool("stop_chronolog", {})


asyncio.run(asyncio.wait_for(main(), timeout=30))
