FROM ubuntu:18.04

# general environment for docker
ENV        DEBIAN_FRONTEND=noninteractive \
           SPACK_ROOT=/root/spack \
           FORCE_UNSAFE_CONFIGURE=1

# install minimal spack dependencies
RUN        apt-get update \
           && apt-get install -y --no-install-recommends \
              autoconf \
              ca-certificates \
              curl \
              environment-modules \
              git \
              build-essential \
              python \
              nano \
              sudo \
              unzip \
           && rm -rf /var/lib/apt/lists/*

RUN        apt-get update \
           && apt-get install -y gcc g++

# setup paths
ENV HOME=/root
ENV PROJECT_DIR=$HOME/source
ENV INSTALL_DIR=$HOME/install
ENV SPACK_DIR=$HOME/spack
ENV SDS_DIR=$HOME/sds

# install spack
RUN echo $INSTALL_DIR && mkdir -p $INSTALL_DIR
RUN git clone https://github.com/spack/spack ${SPACK_DIR}
RUN git clone https://xgitlab.cels.anl.gov/sds/sds-repo.git ${SDS_DIR}
RUN git clone https://github.com/HDFGroup/hcl ${PROJECT_DIR}

ENV spack=${SPACK_DIR}/bin/spack
RUN . ${SPACK_DIR}/share/spack/setup-env.sh
RUN $spack repo add ${SDS_DIR}
RUN $spack repo add ${PROJECT_DIR}/ci/hcl

# install software
ENV HCL_VERSION=dev

#RUN $spack spec "hcl@${HCL_VERSION}"

ENV HCL_SPEC=hcl@${HCL_VERSION}
RUN $spack install --only dependencies ${HCL_SPEC} communication=rpclib

RUN $spack install --only dependencies ${HCL_SPEC} communication=thallium

## Link Software
RUN $spack view symlink -i ${INSTALL_DIR} gcc@8.3.0 mpich@3.3.2 rpclib@2.2.1 mochi-thallium@0.8.3 boost@1.74.0

RUN echo "export PATH=${SPACK_ROOT}/bin:$PATH" >> /root/.bashrc
RUN echo ". $SPACK_ROOT/share/spack/setup-env.sh" >> /root/.bashrc

SHELL ["/bin/bash", "-c"]

RUN apt-get install -y cmake pkg-config