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

CMD ["/bin/bash"]
