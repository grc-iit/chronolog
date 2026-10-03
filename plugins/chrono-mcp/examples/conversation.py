import asyncio
import json
import os
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


async def main():
    params = StdioServerParameters(command="chronolog-mcp", args=["--identity", "example/assistant"], env={
        "CHRONOLOG_CATALOG": os.getenv("CHRONOLOG_CATALOG", "127.0.0.1:50051"),
        "CHRONOLOG_PLAYER": os.getenv("CHRONOLOG_PLAYER", "127.0.0.1:50054")})
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()

            async def call(name, **arguments):
                return json.loads((await session.call_tool(name, arguments)).content[0].text)

            opened = await call("context_open", name="conversation", create=True)
            handle = opened["session_handle"]
            if opened["state"] != "READY":
                print(opened["verdict"])
                await call("context_reconcile", session_handle=handle, operation_ids=["turn-1"])
            await call("context_remember", session_handle=handle, operation_id="turn-1",
                       content=json.dumps({"user": "What happened?", "assistant": "The job finished."}),
                       content_type="application/json")
            print(json.dumps(await call("context_latest", session_handle=handle, n=5), indent=2))
            await call("context_close", session_handle=handle)


asyncio.run(asyncio.wait_for(main(), timeout=30))
