# Runtime image: builds the listed targets in the builder image, then copies only
# the binaries into a clean base. CHRONOLOG_ROLE (visor, keeper, grapher, player)
# picks the binary at start. Build from the repository root:
#   podman build -f deploy/containers/runtime.Containerfile -t chronolog-runtime:local .
ARG BUILDER_IMAGE=chronolog-builder:local

FROM ${BUILDER_IMAGE} AS build
ARG CHRONOLOG_TARGETS="chrono_visor chrono_keeper chrono_player chrono_grapher chronolog_stream_collect chronolog_stream_export"
COPY . /src
RUN cmake --preset release \
    && cmake --build --preset release --parallel 4 --target ${CHRONOLOG_TARGETS} chronolog_client \
    && mkdir -p /out/bin /out/lib \
    && for target in ${CHRONOLOG_TARGETS}; do \
         cp "$(find build/release -type f -name "$target" -perm -u+x | head -n 1)" /out/bin/ ; \
       done \
    && cp -L build/release/client/cpp/libchronolog_client.so.4 /out/lib/

# ubuntu:24.04 index digest resolved from the Docker Hub registry API.
FROM ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3
RUN useradd --system --uid 10001 --no-create-home --shell /usr/sbin/nologin chronolog \
    && mkdir -p /var/lib/chronolog/archive \
    && chown -R 10001:10001 /var/lib/chronolog
COPY --from=build /out/bin/ /usr/local/bin/
COPY --from=build /out/lib/ /usr/local/lib/
COPY deploy/containers/entrypoint.sh /usr/local/bin/chronolog-entrypoint
USER 10001:10001
ENV LD_LIBRARY_PATH=/usr/local/lib \
    CHRONOLOG_ROLE=visor \
    CHRONOLOG_PORT=50051
EXPOSE 50051
ENTRYPOINT ["/usr/local/bin/chronolog-entrypoint"]
