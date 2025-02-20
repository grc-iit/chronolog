# -------------------------------
# Stage 1: Base - Install dependencies, create user, and clone repos
# -------------------------------
FROM ubuntu:22.04 AS base
SHELL ["/bin/bash", "-c"]

ARG USERNAME=grc-iit
ARG UID=1001

ENV DEBIAN_FRONTEND=noninteractive \
    SPACK_ROOT="/home/${USERNAME}/Spack" \
    CHRONOLOG_ROOT="/home/${USERNAME}/ChronoLog" \
    CHRONOLOG_INSTALL="/home/${USERNAME}/chronolog"

RUN apt-get update && apt-get -y install \
    git gcc g++ python3 python3-pip python3-dev jq automake libtool pkgconf \
    libjson-c-dev libboost-dev libcereal-dev sudo libssl-dev && \
    rm -rf /var/lib/apt/lists/*

# Create a non-root user
RUN useradd -m -u ${UID} -s /bin/bash ${USERNAME} && \
    echo "${USERNAME} ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/${USERNAME}

USER ${USERNAME}
WORKDIR /home/${USERNAME}

# Clone ChronoLog and Spack
RUN git clone https://github.com/grc-iit/ChronoLog.git ${CHRONOLOG_ROOT} \
    && cd ${CHRONOLOG_ROOT} && git switch develop

RUN git clone --branch v0.21.2 https://github.com/spack/spack.git ${SPACK_ROOT} 

# -------------------------------
# Stage 2: Build - Compile ChronoLog
# -------------------------------
FROM base AS builder

USER ${USERNAME}
WORKDIR ${CHRONOLOG_ROOT}

# Set up Spack and install dependencies
RUN source ${SPACK_ROOT}/share/spack/setup-env.sh && \
    spack env activate -p . && \
    spack install -v && \
    spack clean --all

# Build ChronoLog
RUN source ${SPACK_ROOT}/share/spack/setup-env.sh && \
    spack env activate -p . && \
    mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release -DINSTALL_DIR=${CHRONOLOG_INSTALL} .. && \
    make all

RUN cd ${CHRONOLOG_ROOT}/deploy \
    && source ${SPACK_ROOT}/share/spack/setup-env.sh \
    && ./local_single_user_deploy.sh -i -w ${CHRONOLOG_INSTALL}/Release

# Clean unnecessary files to reduce image size
RUN rm -rf /home/${USERNAME}/Spack/var/spack/stage \
    && rm -rf /home/${USERNAME}/Spack/opt/spack/.spack-db \
    && rm -rf /home/${USERNAME}/Spack/opt/spack/.spack-cache \
    && rm -rf /home/${USERNAME}/ChronoLog/build \
    && rm -rf /home/${USERNAME}/ChronoLog/.git \
    && rm -rf /home/${USERNAME}/ChronoLog/test

# -------------------------------
# Stage 3: Runtime - Create a smaller final image
# -------------------------------
FROM ubuntu:22.04 AS runtime

ARG USERNAME=grc-iit
ARG UID=1001
ENV SPACK_ROOT="/home/${USERNAME}/Spack" 

# Install only the necessary runtime dependencies
RUN apt-get update && apt-get -y install \
    sudo gcc jq \
    && rm -rf /var/lib/apt/lists/*

# Create user again in the final image
RUN useradd -m -u ${UID} -s /bin/bash ${USERNAME} && \
    echo "${USERNAME} ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/${USERNAME}

USER ${USERNAME}
WORKDIR /home/${USERNAME}

# Copy built files from builder stage
COPY --from=builder /home/${USERNAME}/ChronoLog/deploy /home/${USERNAME}/ChronoLog/deploy
COPY --from=builder /home/${USERNAME}/chronolog /home/${USERNAME}/chronolog
COPY --from=builder /home/${USERNAME}/Spack/opt/spack /home/${USERNAME}/Spack/opt/spack

# Welcome message
CMD ["/bin/bash", "-c", "echo $'+-----------------------------------------------------------------------------------------+\n| ..######..##.....##.########...#######..##....##..#######..##........#######...######.. |\n| .##....##.##.....##.##.....##.##.....##.###...##.##.....##.##.......##.....##.##....##. |\n| .##.......##.....##.##.....##.##.....##.####..##.##.....##.##.......##.....##.##....... |\n| .##.......#########.########..##.....##.##.##.##.##.....##.##.......##.....##.##...#### |\n| .##.......##.....##.##...##...##.....##.##..####.##.....##.##.......##.....##.##....##. |\n| .##....##.##.....##.##....##..##.....##.##...###.##.....##.##.......##.....##.##....##. |\n| ..######..##.....##.##.....##..#######..##....##..#######..########..#######...######.. |\n+-----------------------------------------------------------------------------------------+\n|                                Welcome to ChronoLog                                     |\n|                                                                                         |\n| ChronoLog is a cutting-edge, distributed, and tiered shared log storage ecosystem       |\n| that leverages physical time to ensure total log ordering and elastic scaling           |\n| through intelligent auto-tiering. Designed as a robust foundation for innovation,       |\n| ChronoLog supports advanced plugins including a SQL-like query engine, a streaming      |\n| processor, a log-based key-value store, and a log-based TensorFlow module.              |\n|                                                                                         |\n| Version:                                                                                |\n|   This Docker image provides a lightweight ChronoLog instance for local deployment.     |\n|   It simplifies installation and understanding of the system. For advanced              |\n|   deployment options, including production-ready configurations, please visit:          |\n|                                                                                         |\n| Useful Links:                                                                           |\n|   • Website:    https://www.chronolog.dev                                               |\n|   • Repository: https://github.com/grc-iit/ChronoLog                                    |\n|   • Wiki:       https://github.com/grc-iit/ChronoLog/wiki                               |\n|                                                                                         |\n| Thank you for choosing ChronoLog. Enjoy your session!                                   |\n+-----------------------------------------------------------------------------------------+' && exec bash"]
