"""
ChronoLog Grafana Backend Service

A FastAPI REST service that wraps the ChronoLog Python client
to provide data source capabilities for Grafana.
"""

import argparse
import json
import os
import sys
import logging
import threading
import asyncio
from typing import Optional
from contextlib import asynccontextmanager

from fastapi import FastAPI, HTTPException, Query
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field

# Configure logging
logging.basicConfig(
    level=logging.DEBUG,
    format="%(asctime)s - %(name)s - %(levelname)s - %(message)s"
)
logger = logging.getLogger("chronolog_service")

# Suppress asyncio debug messages (FastAPI/Starlette uses asyncio internally)
# but our endpoints are synchronous, so we don't need to see asyncio debug output
logging.getLogger("asyncio").setLevel(logging.INFO)

# Add the ChronoLog library path if specified
CHRONOLOG_LIB_PATH = os.getenv("CHRONOLOG_LIB_PATH", "")
if CHRONOLOG_LIB_PATH:
    sys.path.insert(0, CHRONOLOG_LIB_PATH)

# Import ChronoLog Python bindings
try:
    import py_chronolog_client as chronolog
    CHRONOLOG_AVAILABLE = True
except ImportError as e:
    logger.warning(f"ChronoLog Python bindings not available: {e}")
    CHRONOLOG_AVAILABLE = False


# Configuration from environment variables or conf file
def _default_conf_path() -> str:
    """Default path for client configuration file."""
    install_path = os.getenv("CHRONOLOG_INSTALL_PATH", os.path.expanduser("~/chronolog-install"))
    return os.path.join(install_path, "chronolog", "conf", "default_client_conf.json")


def load_config_from_file(path: str) -> Optional["ChronoLogConfig"]:
    """
    Load ChronoLog client configuration from a JSON file.

    Expected format matches ChronoLog C++ ClientConfiguration:
    {
      "chrono_client": {
        "VisorClientPortalService": {"rpc": {...}},
        "ClientQueryService": {"rpc": {...}}
      }
    }
    """
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        chrono = data.get("chrono_client", {})
        portal = chrono.get("VisorClientPortalService", {}).get("rpc", {})
        query = chrono.get("ClientQueryService", {}).get("rpc", {})
        return ChronoLogConfig(
            protocol=portal.get("protocol_conf", "ofi+sockets"),
            portal_ip=portal.get("service_ip", "127.0.0.1"),
            portal_port=int(portal.get("service_base_port", 5555)),
            portal_provider_id=int(portal.get("service_provider_id", 55)),
            query_ip=query.get("service_ip", "127.0.0.1"),
            query_port=int(query.get("service_base_port", 5557)),
            query_provider_id=int(query.get("service_provider_id", 57)),
        )
    except (OSError, json.JSONDecodeError, KeyError, TypeError, ValueError) as e:
        logger.warning("Failed to load config from %s: %s", path, e)
        return None


class ChronoLogConfig:
    """Configuration for ChronoLog client connection."""

    def __init__(
        self,
        protocol: str = "ofi+sockets",
        portal_ip: str = "127.0.0.1",
        portal_port: int = 5555,
        portal_provider_id: int = 55,
        query_ip: str = "127.0.0.1",
        query_port: int = 5557,
        query_provider_id: int = 57,
    ):
        self.protocol = protocol
        self.portal_ip = portal_ip
        self.portal_port = portal_port
        self.portal_provider_id = portal_provider_id
        self.query_ip = query_ip
        self.query_port = query_port
        self.query_provider_id = query_provider_id

    @classmethod
    def from_env(cls) -> "ChronoLogConfig":
        """Create config from environment variables."""
        return cls(
            protocol=os.getenv("CHRONOLOG_PROTOCOL", "ofi+sockets"),
            portal_ip=os.getenv("CHRONOLOG_PORTAL_IP", "127.0.0.1"),
            portal_port=int(os.getenv("CHRONOLOG_PORTAL_PORT", "5555")),
            portal_provider_id=int(os.getenv("CHRONOLOG_PORTAL_PROVIDER_ID", "55")),
            query_ip=os.getenv("CHRONOLOG_QUERY_IP", "127.0.0.1"),
            query_port=int(os.getenv("CHRONOLOG_QUERY_PORT", "5557")),
            query_provider_id=int(os.getenv("CHRONOLOG_QUERY_PROVIDER_ID", "57")),
        )


def _resolve_config(conf_file: Optional[str] = None) -> ChronoLogConfig:
    """
    Resolve configuration: conf file > env vars > defaults.
    If conf_file is given and exists, load from it. Otherwise use env vars.
    """
    if conf_file and os.path.isfile(conf_file):
        loaded = load_config_from_file(conf_file)
        if loaded is not None:
            return loaded
        logger.info("Falling back to environment/defaults")
    return ChronoLogConfig.from_env()


def parse_args(args: Optional[list[str]] = None) -> argparse.Namespace:
    """Parse command-line arguments."""
    default_conf = _default_conf_path()
    parser = argparse.ArgumentParser(description="ChronoLog Grafana Backend Service")
    parser.add_argument(
        "-c",
        "--conf",
        metavar="FILE",
        default=None,
        help=f"Path to ChronoLog client configuration JSON (default: {default_conf})",
    )
    parsed = parser.parse_args(args)
    # If --conf not given, use env var or default path (only if file exists)
    if parsed.conf is None:
        parsed.conf = os.getenv("CHRONOLOG_CONF_FILE")
    if parsed.conf is None and os.path.isfile(default_conf):
        parsed.conf = default_conf
    return parsed


# Initialize config at module load: CHRONOLOG_CONF_FILE > default path if exists > env vars
def _init_config() -> ChronoLogConfig:
    conf_file = os.getenv("CHRONOLOG_CONF_FILE")
    if not conf_file:
        default_path = _default_conf_path()
        conf_file = default_path if os.path.isfile(default_path) else None
    if conf_file and os.path.isfile(conf_file):
        loaded = load_config_from_file(conf_file)
        if loaded is not None:
            return loaded
    return ChronoLogConfig.from_env()


config = _init_config()

# Global client instance
client: Optional["chronolog.Client"] = None

# Lock to serialize access to the C++ client (not thread-safe)
# FastAPI's TestClient runs sync endpoints in different threads via anyio,
# so we need to serialize access to prevent segfaults
_client_lock = threading.Lock()


class HealthResponse(BaseModel):
    """Response model for health check."""

    status: str
    chronolog_available: bool
    connected: bool
    

# Pydantic models for API
class ConnectionConfig(BaseModel):
    """Configuration for connecting to ChronoLog service."""

    protocol: str = Field(default="ofi+sockets", description="RPC protocol configuration")
    portal_ip: str = Field(default="127.0.0.1", description="Portal service IP address")
    portal_port: int = Field(default=5555, description="Portal service port")
    portal_provider_id: int = Field(default=55, description="Portal service provider ID")
    query_ip: str = Field(default="127.0.0.1", description="Query service IP address")
    query_port: int = Field(default=5557, description="Query service port")
    query_provider_id: int = Field(default=57, description="Query service provider ID")
    conf_file: Optional[str] = Field(default=None, description="Path to client config JSON (overrides other fields if provided)")


class ConnectionResponse(BaseModel):
    """Response model for connection operations."""

    success: bool
    message: str


class DisconnectionResponse(BaseModel):
    """Response model for connection operations."""

    success: bool
    message: str


class AcquireRequest(BaseModel):
    """Request model for acquiring a story."""

    chronicle_name: str = Field(..., description="Name of the chronicle")
    story_name: str = Field(..., description="Name of the story")
    attrs: dict[str, str] = Field(default_factory=dict, description="Story attributes")


class AcquireResponse(BaseModel):
    """Response model for story acquisition."""

    success: bool
    message: str
    error_code: int = Field(default=0, description="Error code from AcquireStory")


class ReleaseRequest(BaseModel):
    """Request model for releasing a story."""

    chronicle_name: str = Field(..., description="Name of the chronicle")
    story_name: str = Field(..., description="Name of the story")


class ReleaseResponse(BaseModel):
    """Response model for story release."""

    success: bool
    message: str
    error_code: int = Field(default=0, description="Error code from ReleaseStory")


class QueryRequest(BaseModel):
    """Request model for querying story events."""

    chronicle_name: str = Field(..., description="Name of the chronicle")
    story_name: str = Field(..., description="Name of the story")
    start_time: int = Field(..., description="Start time (Unix timestamp in nanoseconds)")
    end_time: int = Field(..., description="End time (Unix timestamp in nanoseconds)")


class EventResponse(BaseModel):
    """Response model for a single event."""

    time: int = Field(..., description="Event timestamp")
    client_id: int = Field(..., description="Client ID that logged the event")
    index: int = Field(..., description="Event index")
    log_record: str = Field(..., description="Log record content")


class QueryResponse(BaseModel):
    """Response model for query results."""

    chronicle_name: str
    story_name: str
    start_time: int
    end_time: int
    events: list[EventResponse]
    total_events: int


def get_client() -> "chronolog.Client":
    """Get the global ChronoLog client instance."""
    global client
    if client is None:
        raise HTTPException(
            status_code=503,
            detail="ChronoLog client not connected. Call /connect first."
        )
    # Note: We can't easily verify if client is actually connected without
    # calling a method, which might cause issues. The client should be
    # connected via /connect endpoint before use.
    return client


@asynccontextmanager
async def lifespan(app: FastAPI):
    """Application lifespan manager for startup/shutdown."""
    logger.info("ChronoLog Grafana Backend starting up...")

    # Auto-connect if environment variables are set
    if CHRONOLOG_AVAILABLE and os.getenv("CHRONOLOG_AUTO_CONNECT", "false").lower() == "true":
        try:
            # connect_client is synchronous, call it directly
            connect_client(ConnectionConfig(
                protocol=config.protocol,
                portal_ip=config.portal_ip,
                portal_port=config.portal_port,
                portal_provider_id=config.portal_provider_id,
                query_ip=config.query_ip,
                query_port=config.query_port,
                query_provider_id=config.query_provider_id,
            ))
            logger.info("Auto-connected to ChronoLog service")
        except Exception as e:
            logger.warning(f"Auto-connect failed: {e}")

    yield

    # Cleanup on shutdown
    global client
    if client is not None:
        try:
            client.Disconnect()
            logger.info("Disconnected from ChronoLog service")
        except Exception as e:
            logger.warning(f"Error disconnecting: {e}")
        client = None


# Create FastAPI application
app = FastAPI(
    title="ChronoLog Grafana Backend",
    description="REST API backend for Grafana ChronoLog data source plugin",
    version="1.0.0",
    lifespan=lifespan,
)

# Add CORS middleware for Grafana access
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],  # Configure appropriately for production
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)


@app.get("/health", response_model=HealthResponse)
def health_check():
    """Health check endpoint for service monitoring."""
    global client
    return HealthResponse(
        status="healthy",
        chronolog_available=CHRONOLOG_AVAILABLE,
        connected=client is not None,
    )


@app.post("/connect", response_model=ConnectionResponse)
async def connect_client(conn_config: Optional[ConnectionConfig] = None):
    """
    Initialize connection to ChronoLog service.

    Resolution order:
      1. conf_file in request body  →  load settings from that JSON file
      2. Module-level config  (loaded from --conf / CHRONOLOG_CONF_FILE / default path / env vars)
    The Pydantic defaults on ConnectionConfig are intentionally ignored so that
    module-level config is always the fallback.
    """
    global client

    if not CHRONOLOG_AVAILABLE:
        raise HTTPException(
            status_code=503,
            detail="ChronoLog Python bindings not available"
        )

    # --- Resolve effective config ---
    # Start from the module-level config (loaded at startup from file / env vars).
    effective = config

    # If the request carries a conf_file, try to load from that file.
    if conn_config is not None and conn_config.conf_file:
        conf_path = conn_config.conf_file
        if os.path.isfile(conf_path):
            loaded = load_config_from_file(conf_path)
            if loaded is not None:
                effective = loaded
                logger.info("Using config loaded from request conf_file: %s", conf_path)
            else:
                logger.warning(
                    "Failed to parse conf_file %s, falling back to module config", conf_path
                )
        else:
            logger.warning(
                "conf_file %s does not exist, falling back to module config", conf_path
            )

    conn_config = ConnectionConfig(
        protocol=effective.protocol,
        portal_ip=effective.portal_ip,
        portal_port=effective.portal_port,
        portal_provider_id=effective.portal_provider_id,
        query_ip=effective.query_ip,
        query_port=effective.query_port,
        query_provider_id=effective.query_provider_id,
    )

    logger.info(
        "Effective connection config: %s://%s:%d (provider %d), query %s:%d (provider %d)",
        conn_config.protocol,
        conn_config.portal_ip,
        conn_config.portal_port,
        conn_config.portal_provider_id,
        conn_config.query_ip,
        conn_config.query_port,
        conn_config.query_provider_id,
    )

    # --- Disconnect existing client before reconnecting ---
    if client is not None:
        logger.info("Disconnecting existing client before reconnecting with new config")
        try:
            def _disconnect():
                with _client_lock:
                    client.Disconnect()
            await asyncio.to_thread(_disconnect)
        except Exception as e:
            logger.warning("Error disconnecting existing client: %s", e)
        client = None

    try:
        # Create configuration objects
        portal_conf = chronolog.ClientPortalServiceConf(
            conn_config.protocol,
            conn_config.portal_ip,
            conn_config.portal_port,
            conn_config.portal_provider_id,
        )
        query_conf = chronolog.ClientQueryServiceConf(
            conn_config.protocol,
            conn_config.query_ip,
            conn_config.query_port,
            conn_config.query_provider_id,
        )

        # Create client with both portal and query configurations
        client = chronolog.Client(portal_conf, query_conf)

        # Connect to the service - run in thread pool to avoid thread-safety issues
        def _connect():
            with _client_lock:
                return client.Connect()
        
        result = await asyncio.to_thread(_connect)
        logger.info(f"Result from client.Connect(): {result}")
        
        if result != 0:
            client = None
            raise HTTPException(
                status_code=503,
                detail=f"Failed to connect to ChronoLog service (error code: {result})"
            )

        # Verify client object is still valid after connection
        if client is None:
            raise HTTPException(
                status_code=503,
                detail="Client became None after connection - this should not happen"
            )
        
        logger.info(
            f"Connected to ChronoLog service at "
            f"{conn_config.portal_ip}:{conn_config.portal_port}"
        )
        return ConnectionResponse(
            success=True,
            message="Successfully connected to ChronoLog service"
        )

    except HTTPException:
        raise
    except Exception as e:
        client = None
        logger.error(f"Connection error: {e}")
        raise HTTPException(
            status_code=503,
            detail=f"Failed to connect to ChronoLog service: {str(e)}"
        )


@app.post("/disconnect", response_model=DisconnectionResponse)
async def disconnect_client():
    """Disconnect from ChronoLog service."""
    global client

    if client is None:
        return DisconnectionResponse(
            success=True,
            message="Not connected"
        )

    try:
        # Run C++ call in thread pool to avoid thread-safety issues
        def _disconnect():
            with _client_lock:
                client.Disconnect()
        
        await asyncio.to_thread(_disconnect)
        logger.info("Disconnected from ChronoLog service")
    except Exception as e:
        logger.warning(f"Error during disconnect: {e}")
    finally:
        client = None

    return DisconnectionResponse(
        success=True,
        message="Disconnected from ChronoLog service"
    )


@app.post("/acquire", response_model=AcquireResponse)
async def acquire_story(request: AcquireRequest):
    """Acquire a story handle for writing or reading."""
    chronolog_client = get_client()
    
    if chronolog_client is None:
        raise HTTPException(
            status_code=503,
            detail="ChronoLog client is None - this should not happen"
        )

    try:
        attrs = request.attrs if request.attrs else dict()

        if not isinstance(request.chronicle_name, str) or not request.chronicle_name:
            raise HTTPException(status_code=400, detail="chronicle_name must be a non-empty string")
        if not isinstance(request.story_name, str) or not request.story_name:
            raise HTTPException(status_code=400, detail="story_name must be a non-empty string")
        if not isinstance(attrs, dict):
            raise HTTPException(status_code=400, detail="attrs must be a dictionary")

        logger.info(
            f"Calling AcquireStory: chronicle='{request.chronicle_name}', "
            f"story='{request.story_name}', attrs={attrs}"
        )
        
        import chronolog_service as cs_module
        if chronolog_client is not cs_module.client:
            logger.error("Client object mismatch - this should not happen!")
            raise HTTPException(
                status_code=500,
                detail="Client object mismatch detected"
            )
        
        def _acquire_story():
            with _client_lock:
                return chronolog_client.AcquireStory(
                    request.chronicle_name,
                    request.story_name,
                    attrs
                )

        result = await asyncio.to_thread(_acquire_story)
        
        logger.info(f"AcquireStory returned: {result}")

        error_code = result[0]
        story_handle = result[1]

        if error_code != 0:
            logger.warning(
                f"AcquireStory returned error code {error_code} for "
                f"{request.chronicle_name}/{request.story_name}"
            )
            return AcquireResponse(
                success=False,
                message=f"Failed to acquire story (error code: {error_code})",
                error_code=error_code,
            )

        logger.info(
            f"Successfully acquired story {request.chronicle_name}/{request.story_name}"
        )
        return AcquireResponse(
            success=True,
            message=f"Successfully acquired story {request.chronicle_name}/{request.story_name}",
            error_code=0,
        )

    except HTTPException:
        raise
    except Exception as e:
        logger.error(
            f"Error acquiring story {request.chronicle_name}/{request.story_name}: {e}"
        )
        raise HTTPException(
            status_code=500,
            detail=f"Failed to acquire story: {str(e)}"
        )


@app.post("/release", response_model=ReleaseResponse)
async def release_story(request: ReleaseRequest):
    """Release a previously acquired story handle."""
    chronolog_client = get_client()

    try:
        # Run C++ call in thread pool to avoid thread-safety issues
        def _release_story():
            with _client_lock:
                return chronolog_client.ReleaseStory(
                    request.chronicle_name,
                    request.story_name
                )
        
        error_code = await asyncio.to_thread(_release_story)

        if error_code != 0:
            logger.warning(
                f"ReleaseStory returned error code {error_code} for "
                f"{request.chronicle_name}/{request.story_name}"
            )
            return ReleaseResponse(
                success=False,
                message=f"Failed to release story (error code: {error_code})",
                error_code=error_code,
            )

        logger.info(
            f"Successfully released story {request.chronicle_name}/{request.story_name}"
        )
        return ReleaseResponse(
            success=True,
            message=f"Successfully released story {request.chronicle_name}/{request.story_name}",
            error_code=0,
        )

    except HTTPException:
        raise
    except Exception as e:
        logger.error(
            f"Error releasing story {request.chronicle_name}/{request.story_name}: {e}"
        )
        raise HTTPException(
            status_code=500,
            detail=f"Failed to release story: {str(e)}"
        )

@app.post("/query", response_model=QueryResponse)
async def query_story(request: QueryRequest):
    """
    Query events from a story within a time range.

    This is the main endpoint for Grafana data source queries.
    """
    # Get client - will raise HTTPException(503) if not connected
    try:
        chronolog_client = get_client()
    except HTTPException:
        # Re-raise HTTPException (like 503 from get_client when not connected)
        raise

    try:
        attrs = {}

        import chronolog_service as cs_module
        if chronolog_client is not cs_module.client:
            logger.error("Client object mismatch in query endpoint!")
            raise HTTPException(
                status_code=500,
                detail="Client object mismatch detected"
            )

        def _acquire_story():
            with _client_lock:
                return chronolog_client.AcquireStory(
                    request.chronicle_name,
                    request.story_name,
                    attrs
                )
        
        acquire_result = await asyncio.to_thread(_acquire_story)
        acquire_error_code = acquire_result[0]
        
        if acquire_error_code != 0:
            logger.error(
                f"Failed to acquire story {request.chronicle_name}/{request.story_name}: "
                f"error code {acquire_error_code}"
            )
            raise HTTPException(
                status_code=500,
                detail=f"Failed to acquire story (error code: {acquire_error_code})"
            )
        
        logger.info(
            f"Successfully acquired story {request.chronicle_name}/{request.story_name}"
        )

        try:
            events = chronolog.EventList()

            def _replay_story():
                with _client_lock:
                    return chronolog_client.ReplayStory(
                        request.chronicle_name,
                        request.story_name,
                        request.start_time,
                        request.end_time,
                        events
                    )
            
            result = await asyncio.to_thread(_replay_story)

            if result != 0:
                logger.warning(
                    f"ReplayStory returned non-zero code: {result} for "
                    f"{request.chronicle_name}/{request.story_name}"
                )

            event_responses = [
                EventResponse(
                    time=event.time(),
                    client_id=event.client_id(),
                    index=event.index(),
                    log_record=event.log_record(),
                )
                for event in events
            ]

            return QueryResponse(
                chronicle_name=request.chronicle_name,
                story_name=request.story_name,
                start_time=request.start_time,
                end_time=request.end_time,
                events=event_responses,
                total_events=len(event_responses),
            )

        finally:
            try:
                def _release_story():
                    with _client_lock:
                        return chronolog_client.ReleaseStory(
                            request.chronicle_name,
                            request.story_name
                        )
                
                release_result = await asyncio.to_thread(_release_story)
                logger.info(
                    f"ReleaseStory returned: {release_result} for "
                    f"{request.chronicle_name}/{request.story_name}"
                )
            except Exception as release_error:
                logger.error(f"Error releasing story: {release_error}")

    except HTTPException:
        raise
    except Exception as e:
        logger.error(
            f"Error querying story {request.chronicle_name}/{request.story_name}: {e}"
        )
        raise HTTPException(
            status_code=500,
            detail=f"Failed to query story: {str(e)}"
        )


# Grafana-specific endpoints for data source integration
@app.get("/")
def root():
    """Root endpoint - returns basic service info for Grafana health check."""
    return {"status": "ok", "service": "chronolog-grafana-backend"}



if __name__ == "__main__":
    import uvicorn

    _cli_args = parse_args()
    if _cli_args.conf:
        loaded = load_config_from_file(_cli_args.conf)
        if loaded is not None:
            config = loaded  # Update module-level config for this process
            logger.info("Loaded config from %s", _cli_args.conf)
        else:
            logger.info("Using environment/default config (conf file load failed)")
    else:
        logger.info("No conf file specified, using environment/default config")

    host = os.getenv("HOST", "0.0.0.0")
    port = int(os.getenv("CHRONOLOG_BACKEND_SERVICE_PORT", "8080"))

    uvicorn.run(
        "chronolog_service:app",
        host=host,
        port=port,
        reload=True,
        log_level="info",
    )

