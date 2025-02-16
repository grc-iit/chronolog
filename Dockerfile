FROM ubuntu:22.04
SHELL ["/bin/bash", "-c"]

# Define build arguments for non-root user creation
ARG USERNAME=grc-iit
ARG UID=1001

# Set environment variables
ENV DEBIAN_FRONTEND=noninteractive
ENV SPACK_ROOT="/home/${USERNAME}/Spack"
ENV CHRONOLOG_ROOT="/home/${USERNAME}/ChronoLog"
ENV CHRONOLOG_INSTALL="/home/${USERNAME}/chronolog"

# -------------------------------
# Install dependencies
# -------------------------------
RUN apt-get update && \
    apt-get -y install \
    git \
    gcc \
    g++ \
    python3 \
    python3-pip \
    cmake \
    make \
    jq \
    automake \
    libtool \
    pkgconf \
    libjson-c-dev \
    libboost-dev \
    libcereal-dev \
    sudo \
    curl \
    wget \
    libssl-dev \
    python3-dev \
    bzip2 \
    xz-utils \
    net-tools \
    lsof \
    htop && \
    rm -rf /var/lib/apt/lists/*

# -------------------------------
# Set up SSH server
# -------------------------------
RUN apt-get update && \
    apt-get -y install --no-install-recommends \
    openssh-server \
    && printf '%s\n' \
    'PermitRootLogin yes' \
    'PasswordAuthentication yes' \
    'PermitEmptyPasswords yes' \
    'UsePAM no' \
    > /etc/ssh/sshd_config.d/auth.conf \
    && printf '%s\n' \
    'Host *' \
    'StrictHostKeyChecking no' \
    > /etc/ssh/ssh_config.d/ignore-host-key.conf \
    && rm -rf /var/lib/apt/lists/*

# -------------------------------
# Create a non-root user with home directory /home/chronolog
# -------------------------------
RUN useradd -m -u ${UID} -s /bin/bash ${USERNAME} && \
    echo "${USERNAME} ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/${USERNAME}

# Switch to that user
USER $USERNAME 

# Set the working directory to the user's home directory
WORKDIR /home/$USERNAME

# Example RUN command executed as '$USERNAME' in /home/$USERNAME
RUN whoami && pwd

# -------------------------------
# Clone ChronoLog Repository
# -------------------------------
RUN git clone https://github.com/grc-iit/ChronoLog.git ${CHRONOLOG_ROOT} \
    && cd ${CHRONOLOG_ROOT} \
    && git switch develop

# -------------------------------
# Install Spack Environment
# -------------------------------
RUN git clone --branch v0.21.2 https://github.com/spack/spack.git ${SPACK_ROOT} \
    && source ${SPACK_ROOT}/share/spack/setup-env.sh \
    && cd ${CHRONOLOG_ROOT} \
    && spack env activate -p . \
    && spack install -v

# -------------------------------
# Build and Install ChronoLog
# -------------------------------
RUN cd ${CHRONOLOG_ROOT} \
    && source ${SPACK_ROOT}/share/spack/setup-env.sh \
    && spack env activate -p . \
    && cmake --version \
    && mkdir build && cd build \
    && cmake -DCMAKE_BUILD_TYPE=Release -DINSTALL_DIR=${CHRONOLOG_INSTALL} .. \
    && make all

RUN cd ${CHRONOLOG_ROOT} \
    && cd build \
    && make install

CMD ["/bin/bash", "-c", "echo $'+-----------------------------------------------------------------------------------------+\n| ..######..##.....##.########...#######..##....##..#######..##........#######...######.. |\n| .##....##.##.....##.##.....##.##.....##.###...##.##.....##.##.......##.....##.##....##. |\n| .##.......##.....##.##.....##.##.....##.####..##.##.....##.##.......##.....##.##....... |\n| .##.......#########.########..##.....##.##.##.##.##.....##.##.......##.....##.##...#### |\n| .##.......##.....##.##...##...##.....##.##..####.##.....##.##.......##.....##.##....##. |\n| .##....##.##.....##.##....##..##.....##.##...###.##.....##.##.......##.....##.##....##. |\n| ..######..##.....##.##.....##..#######..##....##..#######..########..#######...######.. |\n+-----------------------------------------------------------------------------------------+\n|                                Welcome to ChronoLog                                     |\n|                                                                                         |\n| ChronoLog is a cutting-edge, distributed, and tiered shared log storage ecosystem       |\n| that leverages physical time to ensure total log ordering and elastic scaling           |\n| through intelligent auto-tiering. Designed as a robust foundation for innovation,       |\n| ChronoLog supports advanced plugins including a SQL-like query engine, a streaming      |\n| processor, a log-based key-value store, and a log-based TensorFlow module.              |\n|                                                                                         |\n| Version:                                                                                |\n|   This Docker image provides a lightweight ChronoLog instance for local deployment.     |\n|   It simplifies installation and understanding of the system. For advanced              |\n|   deployment options, including production-ready configurations, please visit:          |\n|                                                                                         |\n| Useful Links:                                                                           |\n|   • Website:    https://www.chronolog.dev                                               |\n|   • Repository: https://github.com/grc-iit/ChronoLog                                    |\n|   • Wiki:       https://github.com/grc-iit/ChronoLog/wiki                               |\n|                                                                                         |\n| Thank you for choosing ChronoLog. Enjoy your session!                                   |\n+-----------------------------------------------------------------------------------------+' && exec bash"]