# Runtime image: builds the listed targets in the builder image, then copies only
# the binaries into a clean base. CHRONOLOG_ROLE (visor, keeper, player, stub)
# picks the binary at start. Build from the repository root:
#   podman build -f deploy/containers/runtime.Containerfile -t chronolog-runtime:local .
ARG BUILDER_IMAGE=chronolog-builder:local

FROM ${BUILDER_IMAGE} AS build
ARG CHRONOLOG_TARGETS="chrono_visor chrono_keeper chrono_player chronolog_stub_server"
COPY . /src
RUN cmake --preset release \
    && cmake --build --preset release --parallel 4 --target ${CHRONOLOG_TARGETS} \
    && mkdir /out \
    && for target in ${CHRONOLOG_TARGETS}; do \
         cp "$(find build/release -type f -name "$target" -perm -u+x | head -n 1)" /out/ ; \
       done

# ubuntu:24.04 index digest resolved from the Docker Hub registry API.
FROM ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3
RUN useradd --system --uid 10001 --no-create-home --shell /usr/sbin/nologin chronolog \
    && mkdir -p /var/lib/chronolog \
    && chown 10001:10001 /var/lib/chronolog
COPY --from=build /out/ /usr/local/bin/
COPY deploy/containers/entrypoint.sh /usr/local/bin/chronolog-entrypoint
USER 10001:10001
ENV CHRONOLOG_ROLE=stub \
    CHRONOLOG_PORT=50051
EXPOSE 50051
ENTRYPOINT ["/usr/local/bin/chronolog-entrypoint"]
