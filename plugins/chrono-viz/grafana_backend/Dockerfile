# ChronoLog Grafana Backend Dockerfile
FROM python:3.11-slim

# Create non-root user for security
RUN groupadd -r chronolog && useradd -r -g chronolog chronolog

# Set working directory
WORKDIR /app

# Install system dependencies including OpenSSL 3.3.0 or compatible version
RUN apt-get update && apt-get install -y --no-install-recommends \
    libfabric1 \
    libssl3 \
    openssl \
    libatomic1 \
    && rm -rf /var/lib/apt/lists/*

# Copy requirements first for better caching
COPY requirements.txt .

# Install Python dependencies
RUN pip install --no-cache-dir -r requirements.txt

# Copy application code
COPY chronolog_service.py .

# Create directory for ChronoLog library
RUN mkdir -p /opt/chronolog/lib && chown -R chronolog:chronolog /opt/chronolog

# Environment variables (CHRONOLOG_BACKEND_SERVICE_PORT overridable at runtime, default 8080)
ENV HOST=0.0.0.0
ENV CHRONOLOG_BACKEND_SERVICE_PORT=8080
ENV CHRONOLOG_LIB_PATH=/opt/chronolog/lib
ENV PYTHONPATH=/opt/chronolog/lib:$PYTHONPATH
# Prepend system lib paths to ensure container's OpenSSL takes precedence over mounted libs
ENV LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu:/usr/local/lib:/opt/chronolog/lib:$LD_LIBRARY_PATH

# Switch to non-root user
USER chronolog

# Expose default port (actual port set by CHRONOLOG_BACKEND_SERVICE_PORT at runtime)
EXPOSE 8080

# Health check (port from CHRONOLOG_BACKEND_SERVICE_PORT at runtime)
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD sh -c 'python -c "import urllib.request, os; p=os.environ.get(\"CHRONOLOG_BACKEND_SERVICE_PORT\", \"8080\"); urllib.request.urlopen(\"http://localhost:\"+p+\"/health\")"' || exit 1

# Run the application (port read from CHRONOLOG_BACKEND_SERVICE_PORT in chronolog_service.py)
CMD ["python", "chronolog_service.py"]

