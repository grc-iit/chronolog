# Runtime image: builds chronolog_stub_server in the builder image, then copies
# only the binary into a clean base. Build from the repository root:
#   podman build -f deploy/containers/runtime.Containerfile -t chronolog-runtime:local .
ARG BUILDER_IMAGE=chronolog-builder:local

FROM ${BUILDER_IMAGE} AS build
COPY . /src
RUN cmake --preset release \
    && cmake --build --preset release --parallel --target chronolog_stub_server

# ubuntu:24.04 index digest resolved from the Docker Hub registry API.
FROM ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3
RUN useradd --system --uid 10001 --no-create-home --shell /usr/sbin/nologin chronolog
COPY --from=build /src/build/release/tools/stub_server/chronolog_stub_server /usr/local/bin/chronolog_stub_server
USER 10001:10001
ENV CHRONOLOG_PORT=50051
EXPOSE 50051
ENTRYPOINT ["/usr/local/bin/chronolog_stub_server"]
