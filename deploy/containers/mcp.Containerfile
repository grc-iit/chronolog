FROM python:3.12-slim-trixie
COPY chronolog-4.0.0-*.whl chronolog_mcp-4.0.0-*.whl /wheels/
RUN python -m venv /opt/build/venv \
    && /opt/build/venv/bin/pip install --no-cache-dir /wheels/*.whl \
    && rm -rf /wheels
ENV PYTHONDONTWRITEBYTECODE=1
USER 10001:10001
ENTRYPOINT ["/opt/build/venv/bin/chronolog-mcp"]
CMD ["--http", "--host", "0.0.0.0", "--port", "8000"]
