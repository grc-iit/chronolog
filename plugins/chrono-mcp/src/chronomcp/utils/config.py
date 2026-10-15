# config.py
import os
from dotenv import load_dotenv
import logging
import py_chronolog_client
from fastmcp import FastMCP

# load .env and set up logging
load_dotenv()
logging.basicConfig(level=logging.WARNING)

# ChronoLog client return codes the tools act on (client_errcode.h)
CL_ERR_CHRONICLE_EXISTS = -6
CL_ERR_PARTIAL_RESULT = -18

# ChronoLog connection settings
CHRONO_PROTOCOL = os.getenv("CHRONO_PROTOCOL", "ofi+sockets")
CHRONO_HOST = os.getenv("CHRONO_HOST", "127.0.0.1")
CHRONO_PORT = int(os.getenv("CHRONO_PORT", 5555))
CHRONO_TIMEOUT = int(os.getenv("CHRONO_TIMEOUT", 55))
DEFAULT_CHRONICLE = os.getenv("CHRONICLE_NAME", "LLM")
DEFAULT_STORY = os.getenv("STORY_NAME", "conversation")

# ChronoPlayer query service, for retrieve: the address and port the client
# listens on for replay answers (ClientQueryService in a client configuration).
# The host is this machine's address as the player reaches it, not the visor's.
# The port must be free: two clients on one host need different ones (a client
# whose port is taken crashes, see issue #719).
CHRONO_QUERY_HOST = os.getenv("CHRONO_QUERY_HOST", "127.0.0.1")
CHRONO_QUERY_PORT = int(os.getenv("CHRONO_QUERY_PORT", 5557))
CHRONO_QUERY_PROVIDER_ID = int(os.getenv("CHRONO_QUERY_PROVIDER_ID", 57))


def make_client():
    """The ChronoLog client, in writer + reader mode so retrieve can replay a
    story through ChronoPlayer. The client library keeps one client per
    process, whose mode the first one sets, so this is the only one."""
    client_conf = py_chronolog_client.ClientPortalServiceConf(
        CHRONO_PROTOCOL, CHRONO_HOST, CHRONO_PORT, CHRONO_TIMEOUT
    )
    query_conf = py_chronolog_client.ClientQueryServiceConf(
        CHRONO_PROTOCOL, CHRONO_QUERY_HOST, CHRONO_QUERY_PORT, CHRONO_QUERY_PROVIDER_ID
    )
    return py_chronolog_client.Client(client_conf, query_conf)


_client = None


def get_client():
    """The client, made at the first tool call: it listens on the query port,
    which importing this module should not take."""
    global _client
    if _client is None:
        _client = make_client()
    return _client


# MCP server instance
mcp: FastMCP = FastMCP(
    "chronolog",
    instructions=(
        "Manages ChronoLog distributed logging system. "
        "Record events, query logs by time range, and monitor log status."
    ),
)

# session state
_active_chronicle = ""
_active_story = ""
_story_handle = None
