# Use scs-lab/base:1.0 as base image
FROM spack/ubuntu-jammy:latest as coeus-builder
ENV DOCKER_TAG=0.2

# What we want to install and how we want to install it
# is specified in a manifest file (spack.yaml)
RUN mkdir /opt/spack-environment \
&&  (echo "spack:" \
&&   echo "  specs:" \
&&   echo "  - cmake@3.25.1" \
&&   echo "  - mpich@4.0.2~fortran +verbs +argobots ^libfabric@1.16.1 fabrics=mlx,rxd,rxm,shm,sockets,tcp,udp,verbs" \
&&   echo "  - mochi-thallium@0.10.1 ^libfabric@1.16.1 fabrics=mlx,rxd,rxm,shm,sockets,tcp,udp,verbs" \
&&   echo "  - mochi-margo@0.13 ^libfabric@1.16.1 fabrics=mlx,rxd,rxm,shm,sockets,tcp,udp,verbs" \
&&   echo "  - mercury@2.2.0~boostsys ^libfabric@1.16.1 fabrics=mlx,rxd,rxm,shm,sockets,tcp,udp,verbs" \
&&   echo "  - boost@1.80.0" \
&&   echo "  concretizer:" \
&&   echo "    unify: true" \
&&   echo "  config:" \
&&   echo "    install_tree: /opt/software" \
&&   echo "  view: /opt/view") > /opt/spack-environment/spack.yaml

# Install the software, remove unnecessary deps
RUN cd /opt/spack-environment && spack env activate . && spack install --fail-fast && spack gc -y

FROM ubuntu:22.04
ENV DEBIAN_FRONTEND="noninteractive"

RUN apt-get update -y && apt-get upgrade -y
RUN apt-get install -y pkg-config cmake build-essential environment-modules gfortran git python3 gdb

COPY --from=coeus-builder /opt/spack-environment /opt/spack-environment
COPY --from=coeus-builder /opt/software /opt/software
COPY --from=coeus-builder /opt/view /usr