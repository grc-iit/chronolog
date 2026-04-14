"""
Integration tests for ChronoLog Grafana Backend Service

Tests the FastAPI REST API endpoints with real ChronoLog Python client.
Requires ChronoLog services (ChronoVisor, ChronoKeeper) to be running.

Environment Variables for Connection:
    CHRONOLOG_PORTAL_IP: IP address of ChronoVisor portal (default: 127.0.0.1)
    CHRONOLOG_PORTAL_PORT: Port of ChronoVisor portal (default: 5555)
    CHRONOLOG_PORTAL_PROVIDER_ID: Portal provider ID (default: 55)
    CHRONOLOG_QUERY_IP: IP address of query service (default: 127.0.0.1)
    CHRONOLOG_QUERY_PORT: Port of query service (default: 5557)
    CHRONOLOG_QUERY_PROVIDER_ID: Query provider ID (default: 57)

Environment Variables for Testing:
    TEST_CHRONICLE_NAME: Chronicle name for query tests (default: test_chronicle)
    TEST_STORY_NAME: Story name for query tests (default: test_story)

Note: Tests for listing chronicles and stories are currently skipped as those
      APIs are not yet implemented in the backend.
"""

import pytest
import os
import asyncio
import httpx
from httpx import ASGITransport


class TestChronoLogService:
    """Integration test suite for ChronoLog Grafana backend service.
    
    Test Organization:
    ==================
    All tests are self-contained and follow the proper ChronoLog lifecycle:
    
    1. Connection Tests:
       - test_connect_disconnect_success: connect -> disconnect -> verify
    
    2. Acquire/Release Tests:
       - test_acquire_release_success: connect -> acquire -> release -> disconnect

    3. Query Tests:
       - test_query_story_success: connect -> acquire -> query -> release -> disconnect
       - test_multiple_queries_same_connection: connect -> acquire -> query1 -> release1 -> acquire -> query2 -> release2 -> acquire -> query3 -> release3 -> disconnect

    4. Validation Tests:
       - test_query_validation_missing_fields: Test request validation
    
    Each test starts with a clean state (setup clears any existing connections).
    """

    @pytest.fixture(autouse=True)
    def setup(self, request):
        """Setup test fixtures before each test."""
        import logging
        # Suppress asyncio debug messages (FastAPI uses asyncio internally)
        logging.getLogger("asyncio").setLevel(logging.INFO)
        
        # Log which test is running
        test_name = request.node.name
        print(f"\n{'='*60}")
        print(f"Running test: {test_name}")
        print(f"{'='*60}")
        
        import chronolog_service
        
        # Disconnect and fully clear any existing connection
        # CRITICAL: Must fully clean up the old client before each test
        # to avoid C++ state issues that cause segfaults
        if chronolog_service.client is not None:
            try:
                # Try to disconnect synchronously (if client is still valid)
                chronolog_service.client.Disconnect()
            except:
                pass
            finally:
                # Explicitly set to None and force garbage collection
                # This ensures the C++ object is fully destroyed
                chronolog_service.client = None
                import gc
                gc.collect()
                # Small delay to ensure C++ cleanup completes
                import time
                time.sleep(0.05)
        
        # Create test client using httpx AsyncClient with ASGITransport
        # IMPORTANT: TestClient uses anyio.from_thread.start_blocking_portal which runs
        # sync endpoints in a separate thread, causing segfaults with C++ objects.
        # Using httpx.AsyncClient with ASGITransport and running it synchronously
        # via asyncio.run() should avoid the threading issue.
        transport = ASGITransport(app=chronolog_service.app)
        self.async_client = httpx.AsyncClient(transport=transport, base_url="http://testserver", timeout=30.0)
        
        # Create a wrapper to run async client calls synchronously
        # This ensures we're in the same thread context
        class SyncClientWrapper:
            def __init__(self, async_client):
                self.async_client = async_client
                self._loop = None
            
            def _get_loop(self):
                if self._loop is None or self._loop.is_closed():
                    self._loop = asyncio.new_event_loop()
                    asyncio.set_event_loop(self._loop)
                return self._loop
            
            def get(self, url, **kwargs):
                loop = self._get_loop()
                return loop.run_until_complete(self.async_client.get(url, **kwargs))
            
            def post(self, url, **kwargs):
                loop = self._get_loop()
                return loop.run_until_complete(self.async_client.post(url, **kwargs))
            
            def close(self):
                if self._loop and not self._loop.is_closed():
                    self._loop.run_until_complete(self.async_client.aclose())
                    self._loop.close()
        
        self.test_client = SyncClientWrapper(self.async_client)
        
        # Get connection parameters from environment or use defaults
        self.conn_params = {
            "protocol": os.getenv("CHRONOLOG_PROTOCOL", "ofi+sockets"),
            "portal_ip": os.getenv("CHRONOLOG_PORTAL_IP", "127.0.0.1"),
            "portal_port": int(os.getenv("CHRONOLOG_PORTAL_PORT", "5555")),
            "portal_provider_id": int(os.getenv("CHRONOLOG_PORTAL_PROVIDER_ID", "55")),
            "query_ip": os.getenv("CHRONOLOG_QUERY_IP", "127.0.0.1"),
            "query_port": int(os.getenv("CHRONOLOG_QUERY_PORT", "5557")),
            "query_provider_id": int(os.getenv("CHRONOLOG_QUERY_PROVIDER_ID", "57"))
        }
        
        yield
        
        # Cleanup: close httpx client and fully disconnect after each test
        self.test_client.close()
        import chronolog_service
        import gc
        if chronolog_service.client is not None:
            try:
                # Try to disconnect synchronously
                chronolog_service.client.Disconnect()
            except Exception:
                pass
            finally:
                # Explicitly set to None and force garbage collection
                # This ensures the C++ object is fully destroyed before next test
                chronolog_service.client = None
                gc.collect()
                # Small delay to ensure C++ cleanup completes
                import time
                time.sleep(0.05)

    # ===================================================================
    # Tests Without Connection Required
    # ===================================================================
    
    def test_health_check_not_connected(self):
        """Test health check when not connected to ChronoLog.
        
        Lifecycle: No connection needed - just check health endpoint
        """
        response = self.test_client.get("/health")
        
        assert response.status_code == 200
        data = response.json()
        assert data["status"] == "healthy"
        assert data["chronolog_available"] is True
        assert data["connected"] is False

    def test_root_endpoint(self):
        """Test root endpoint returns service info.
        
        Lifecycle: No connection needed - just check root endpoint
        """
        response = self.test_client.get("/")
        
        assert response.status_code == 200
        data = response.json()
        assert data["status"] == "ok"
        assert data["service"] == "chronolog-grafana-backend"

    # ===================================================================
    # Connection/Disconnection Tests
    # ===================================================================
    
    # def test_connect_disconnect_success(self):
    #     """Test successful connection to ChronoLog service.
        
    #     Lifecycle: connect -> verify -> disconnect
    #     """
    #     # Connect to ChronoLog
    #     response = self.test_client.post("/connect", json=self.conn_params)
        
    #     print(f"Connection response: {response.json()}")
    #     print(f"Status code: {response.status_code}")
        
    #     assert response.status_code == 200
    #     data = response.json()
    #     assert data["success"] is True
    #     assert "Successfully connected" in data["message"]
        
    #     # Verify health check shows connected
    #     health = self.test_client.get("/health")
    #     assert health.json()["connected"] is True
        
    #     # Disconnect (explicit cleanup)
    #     disconnect_response = self.test_client.post("/disconnect")
    #     assert disconnect_response.status_code == 200
    #     print("Disconnected successfully")

    # ===================================================================
    # Acquire/Release Tests
    # ===================================================================

    # def test_acquire_release_success(self):
    #     """Test successful acquire/release of a story.
        
    #     Lifecycle: connect -> acquire -> release -> disconnect
    #     """
    #     # 1. Connect to ChronoLog
    #     conn_response = self.test_client.post("/connect", json=self.conn_params)
    #     assert conn_response.status_code == 200
    #     print("Connected successfully")
        
    #     # Small delay to ensure connection is fully established
    #     # This helps avoid potential timing issues with C++ client initialization
    #     import time
    #     time.sleep(0.1)
        
    #     # 2. Get chronicle/story names from environment or use defaults
    #     chronicle_name = os.getenv("TEST_CHRONICLE_NAME", "kfeng-EVO-X2")
    #     story_name = os.getenv("TEST_STORY_NAME", "cpu_usage")
    #     print(f"Acquiring story: {chronicle_name}/{story_name}")

    #     # 3. Acquire the story
    #     print(f"[TEST] About to call /acquire endpoint...")
    #     acquire_response = self.test_client.post("/acquire", json={
    #         "chronicle_name": chronicle_name,
    #         "story_name": story_name
    #     })
    #     print(f"[TEST] /acquire response status: {acquire_response.status_code}")
    #     assert acquire_response.status_code == 200
    #     print("Acquired successfully")
        
    #     # 4. Release the story
    #     release_response = self.test_client.post("/release", json={
    #         "chronicle_name": chronicle_name,
    #         "story_name": story_name
    #     })
    #     assert release_response.status_code == 200
    #     print("Released successfully")
        
    #     # 5. Disconnect (explicit cleanup)
    #     disconnect_response = self.test_client.post("/disconnect")
    #     assert disconnect_response.status_code == 200
    #     print("Disconnected successfully")

    # ===================================================================
    # Query Tests
    # ===================================================================
    
    def test_query_story_success(self):
        """Test successful story query with real data.
        
        Lifecycle: connect -> query (acquire->replay->release handled by backend) -> disconnect
        
        Note: The backend service automatically handles acquire/release within the query endpoint.
        """
        # 1. Connect to ChronoLog
        conn_response = self.test_client.post("/connect", json=self.conn_params)
        assert conn_response.status_code == 200
        print("Connected successfully")
        
        # 2. Get chronicle/story names from environment or use defaults
        chronicle_name = os.getenv("TEST_CHRONICLE_NAME", "kfeng-EVO-X2")
        story_name = os.getenv("TEST_STORY_NAME", "cpu_usage")
        
        # 3. Acquire the story
        acquire_response = self.test_client.post("/acquire", json={
            "chronicle_name": chronicle_name,
            "story_name": story_name
        })
        assert acquire_response.status_code == 200
        print("Acquired successfully")
        
        # 4. Release the story
        release_response = self.test_client.post("/release", json={
            "chronicle_name": chronicle_name,
            "story_name": story_name
        })
        assert release_response.status_code == 200
        print("Released successfully")

        # 5. Query the story (backend handles: acquire -> replay -> release)
        print(f"Querying story: {chronicle_name}/{story_name}")
        response = self.test_client.post("/query", json={
            "chronicle_name": chronicle_name,
            "story_name": story_name,
            "start_time": 1,  # Query from beginning
            "end_time": 9999999999999999999  # To far future
        })
        
        # Verify query response
        assert response.status_code == 200
        data = response.json()
        assert data["chronicle_name"] == chronicle_name
        assert data["story_name"] == story_name
        assert "events" in data
        assert "total_events" in data
        assert isinstance(data["events"], list)
        print(f"Found {data['total_events']} events in {chronicle_name}/{story_name}")
        
        # Print some events if available
        if data['total_events'] > 0:
            print(f"First event: time={data['events'][0]['time']}, "
                  f"log_record={data['events'][0]['log_record']}")
        
        # 6. Disconnect (explicit cleanup)
        disconnect_response = self.test_client.post("/disconnect")
        assert disconnect_response.status_code == 200
        print("Disconnected successfully")
    
    def test_query_not_connected(self):
        """Test query endpoint when not connected to ChronoLog.
        
        Should fail without connection.
        """
        chronicle_name = os.getenv("TEST_CHRONICLE_NAME", "kfeng-EVO-X2")
        story_name = os.getenv("TEST_STORY_NAME", "cpu_usage")
        
        response = self.test_client.post("/query", json={
            "chronicle_name": chronicle_name,
            "story_name": story_name,
            "start_time": 1,
            "end_time": 9999999999999999999
        })
        
        assert response.status_code == 503
        assert "not connected" in response.json()["detail"].lower()
        print("Query correctly failed when not connected")

    def test_multiple_queries_same_connection(self):
        """Test multiple queries using the same connection.
        
        Lifecycle: connect -> query1 -> query2 -> query3 -> disconnect
        
        Tests that acquire/release pairs work correctly for multiple queries.
        """
        # 1. Connect to ChronoLog
        conn_response = self.test_client.post("/connect", json=self.conn_params)
        assert conn_response.status_code == 200
        print("Connected successfully")
        
        chronicle_name = os.getenv("TEST_CHRONICLE_NAME", "kfeng-EVO-X2")
        story_name = os.getenv("TEST_STORY_NAME", "cpu_usage")
        
        # 2. Execute multiple queries (each handles acquire/release internally)
        for i in range(3):
            print(f"\nQuery {i+1}: {chronicle_name}/{story_name}")
            response = self.test_client.post("/query", json={
                "chronicle_name": chronicle_name,
                "story_name": story_name,
                "start_time": 1,
                "end_time": 9999999999999999999
            })
            
            assert response.status_code == 200
            data = response.json()
            print(f"Query {i+1} returned {data['total_events']} events")
        
        # 3. Disconnect
        disconnect_response = self.test_client.post("/disconnect")
        assert disconnect_response.status_code == 200
        print("Disconnected successfully")

    # ===================================================================
    # Additional Endpoint Tests
    # ===================================================================
    
    # def test_search_endpoint_not_connected(self):
    #     """Test search endpoint when not connected.
        
    #     Note: Search endpoint currently returns empty list (not fully implemented).
    #     """
    #     response = self.test_client.post("/search")
        
    #     assert response.status_code == 200
    #     assert response.json() == []

    # ===================================================================
    # Validation Tests
    # ===================================================================
    
    def test_query_validation_missing_fields(self):
        """Test query endpoint with missing required fields.
        
        Should return validation error (422) for incomplete request.
        """
        response = self.test_client.post("/query", json={
            "chronicle_name": "test_chronicle"
            # Missing story_name, start_time, end_time
        })
        
        assert response.status_code == 422  # Validation error


class TestConnectionConfig:
    """Test ConnectionConfig model."""

    def test_connection_config_defaults(self):
        """Test ConnectionConfig with default values."""
        from chronolog_service import ConnectionConfig
        
        config = ConnectionConfig()
        assert config.protocol == "ofi+sockets"
        assert config.portal_ip == "127.0.0.1"
        assert config.portal_port == 5555
        assert config.portal_provider_id == 55
        assert config.query_ip == "127.0.0.1"
        assert config.query_port == 5557
        assert config.query_provider_id == 57

    def test_connection_config_custom_values(self):
        """Test ConnectionConfig with custom values."""
        from chronolog_service import ConnectionConfig
        
        config = ConnectionConfig(
            protocol="custom_protocol",
            portal_ip="192.168.1.100",
            portal_port=6000,
            portal_provider_id=100,
            query_ip="192.168.1.101",
            query_port=6001,
            query_provider_id=101
        )
        assert config.protocol == "custom_protocol"
        assert config.portal_ip == "192.168.1.100"
        assert config.portal_port == 6000
        assert config.portal_provider_id == 100


class TestRequestModels:
    """Test request/response models."""

    def test_query_request_model(self):
        """Test QueryRequest model."""
        from chronolog_service import QueryRequest
        
        request = QueryRequest(
            chronicle_name="kfeng-EVO-X2",
            story_name="cpu_usage",
            start_time=1000000000000000000,
            end_time=2000000000000000000
        )
        assert request.chronicle_name == "kfeng-EVO-X2"
        assert request.story_name == "cpu_usage"
        assert request.start_time == 1000000000000000000
        assert request.end_time == 2000000000000000000

    def test_event_response_model(self):
        """Test EventResponse model."""
        from chronolog_service import EventResponse
        
        event = EventResponse(
            time=1000000000000000000,
            client_id=1,
            index=0,
            log_record="Test log entry"
        )
        assert event.time == 1000000000000000000
        assert event.client_id == 1
        assert event.index == 0
        assert event.log_record == "Test log entry"


if __name__ == "__main__":
    pytest.main([__file__, "-v", "-s"])
