"""
ChronoLog Grafana Backend Service

A FastAPI REST service that wraps the ChronoLog Python client
to provide data source capabilities for Grafana.
"""

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


# Configuration from environment variables
class ChronoLogConfig:
    """Configuration for ChronoLog client connection."""

    def __init__(self):
        self.protocol = os.getenv("CHRONOLOG_PROTOCOL", "ofi+sockets")
        self.portal_ip = os.getenv("CHRONOLOG_PORTAL_IP", "127.0.0.1")
        self.portal_port = int(os.getenv("CHRONOLOG_PORTAL_PORT", "5555"))
        self.portal_provider_id = int(os.getenv("CHRONOLOG_PORTAL_PROVIDER_ID", "55"))
        self.query_ip = os.getenv("CHRONOLOG_QUERY_IP", "127.0.0.1")
        self.query_port = int(os.getenv("CHRONOLOG_QUERY_PORT", "5557"))
        self.query_provider_id = int(os.getenv("CHRONOLOG_QUERY_PROVIDER_ID", "57"))


config = ChronoLogConfig()

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
    flags: int = Field(default=1, description="Acquisition flags")


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

    Uses provided configuration or falls back to environment variables.
    """
    global client

    if not CHRONOLOG_AVAILABLE:
        raise HTTPException(
            status_code=503,
            detail="ChronoLog Python bindings not available"
        )

    # Use provided config or defaults from environment
    if conn_config is None:
        conn_config = ConnectionConfig(
            protocol=config.protocol,
            portal_ip=config.portal_ip,
            portal_port=config.portal_port,
            portal_provider_id=config.portal_provider_id,
            query_ip=config.query_ip,
            query_port=config.query_port,
            query_provider_id=config.query_provider_id,
        )

    # Disconnect and fully destroy existing client if any
    # CRITICAL: Must fully clean up the old client before creating a new one
    # to avoid C++ state issues that cause segfaults in subsequent tests
    if client is not None:
        try:
            # Disconnect first
            def _disconnect_old():
                with _client_lock:
                    if client is not None:
                        client.Disconnect()
            await asyncio.to_thread(_disconnect_old)
        except Exception as e:
            logger.warning(f"Error disconnecting old client: {e}")
        finally:
            # Explicitly set to None and let Python GC clean it up
            # This ensures the C++ object is fully destroyed
            client = None
            # Force garbage collection to ensure C++ object is destroyed
            import gc
            gc.collect()

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
        flags = request.flags if request.flags is not None else 1
        
        if not isinstance(request.chronicle_name, str) or not request.chronicle_name:
            raise HTTPException(status_code=400, detail="chronicle_name must be a non-empty string")
        if not isinstance(request.story_name, str) or not request.story_name:
            raise HTTPException(status_code=400, detail="story_name must be a non-empty string")
        if not isinstance(attrs, dict):
            raise HTTPException(status_code=400, detail="attrs must be a dictionary")
        if not isinstance(flags, int):
            raise HTTPException(status_code=400, detail="flags must be an integer")
        
        logger.info(
            f"Calling AcquireStory: chronicle='{request.chronicle_name}', "
            f"story='{request.story_name}', attrs={attrs}, flags={flags}"
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
                    attrs,
                    flags
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
        flags = 1
        
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
                    attrs,
                    flags
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

    host = os.getenv("HOST", "0.0.0.0")
    port = int(os.getenv("PORT", "8080"))

    uvicorn.run(
        "chronolog_service:app",
        host=host,
        port=port,
        reload=True,
        log_level="info",
    )

