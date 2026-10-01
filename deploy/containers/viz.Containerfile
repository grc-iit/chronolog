FROM python:3.12-slim-trixie
COPY chronolog-4.0.0-cp312-abi3-linux_x86_64.whl chronolog_viz-4.0.0-py3-none-any.whl /wheels/
RUN python -m venv /opt/build/venv \
    && /opt/build/venv/bin/pip install --no-cache-dir /wheels/*.whl \
    && rm -rf /wheels
ENV PYTHONDONTWRITEBYTECODE=1
USER 10001:10001
ENTRYPOINT ["/opt/build/venv/bin/python", "-m", "uvicorn", "chronolog_viz:app"]
CMD ["--host", "0.0.0.0", "--port", "8087"]
