# Runtime image from binaries built natively with the dev preset. The build context
# is the staging directory that tests/smoke/run.sh fills with the binaries
# and entrypoint.sh, so no toolchain or dependency build happens in the container:
#   podman build -f deploy/containers/runtime-local.Containerfile -t chronolog-runtime-local:dev build/image-stage
# GitHub CI uses runtime.Containerfile instead.
# ubuntu:24.04 index digest resolved from the Docker Hub registry API.
FROM ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3
RUN useradd --system --uid 10001 --no-create-home --shell /usr/sbin/nologin chronolog \
    && mkdir -p /var/lib/chronolog/archive \
    && chown -R 10001:10001 /var/lib/chronolog
# Ships the Visor, Keeper, Player and Grapher staged by run.sh.
COPY chrono_* chronolog_stream_* /usr/local/bin/
COPY libchronolog_client.so.4 /usr/local/lib/
COPY entrypoint.sh /usr/local/bin/chronolog-entrypoint
USER 10001:10001
ENV LD_LIBRARY_PATH=/usr/local/lib \
    CHRONOLOG_ROLE=visor \
    CHRONOLOG_PORT=50051
EXPOSE 50051
ENTRYPOINT ["/usr/local/bin/chronolog-entrypoint"]
