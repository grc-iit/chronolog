FROM ubuntu:24.04

SHELL ["/bin/bash", "-c"]

ARG USERNAME=grc-iit
ARG UID=1001

# Set noninteractive frontend
ENV DEBIAN_FRONTEND=noninteractive
ENV DEBCONF_NOWARNINGS=yes
ENV TZ=America/Chicago

# Install system dependencies
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get -y install \
    gcc g++ automake cmake libtool pkgconf \
    libjson-c-dev libboost-dev libcereal-dev \
    git vim fuse sudo curl wget \
    libfuse-dev libssl-dev \
    python3 python3-dev python3-pip \
    bzip2 xz-utils jq net-tools lsof htop pssh procps bind9-dnsutils chrpath \
    build-essential unzip tar autoconf

# Install SSH server and client with configuration
RUN apt-get -y install --no-install-recommends \
    openssh-server openssh-client \
  && printf '%s\n' \
    'PermitRootLogin yes' \
    'PasswordAuthentication yes' \
    'PermitEmptyPasswords yes' \
    'UsePAM no' \
    > /etc/ssh/sshd_config.d/auth.conf \
  && printf '%s\n' \
    'Host *' \
    '    StrictHostKeyChecking no' \
    > /etc/ssh/ssh_config.d/ignore-host-key.conf \
  && pip3 install --break-system-packages pssh

# Create grc-iit user
RUN id $UID && userdel $(id -un $UID) || : \
 && useradd -m -u $UID -s /bin/bash $USERNAME \
 && mkdir -p /etc/sudoers.d \
 && echo "$USERNAME ALL=(ALL:ALL) NOPASSWD: ALL" >> /etc/sudoers.d/$USERNAME \
 && passwd -d $USERNAME

# Switch to user to install Spack in their home directory
USER $USERNAME
WORKDIR /home/$USERNAME

# Get ChronoLog
RUN git clone https://github.com/grc-iit/ChronoLog.git chronolog-repo \
 && cd chronolog-repo \
 && git switch develop \
 && git pull

# Install Spack and build/install ChronoLog
RUN git clone --branch v0.21.2 https://github.com/spack/spack.git ~/spack \
 && export SPACK_ROOT=$(pwd)/spack \
 && source spack/share/spack/setup-env.sh \
 && cd chronolog-repo \
 && ./tools/deploy/ChronoLog/local_single_user_deploy.sh -b \
 && ./tools/deploy/ChronoLog/local_single_user_deploy.sh -i

# Source Spack and set USER on each shell
RUN printf '%s\n' \
    'source ~/spack/share/spack/setup-env.sh' \
    'export USER=grc-iit' \
    >> ~/.bashrc

# Welcome message
CMD ["/bin/bash", "-c", "echo $'+-----------------------------------------------------------------------------------------+\n| ..######..##.....##.########...#######..##....##..#######..##........#######...######.. |\n| .##....##.##.....##.##.....##.##.....##.###...##.##.....##.##.......##.....##.##....##. |\n| .##.......##.....##.##.....##.##.....##.####..##.##.....##.##.......##.....##.##....... |\n| .##.......#########.########..##.....##.##.##.##.##.....##.##.......##.....##.##...#### |\n| .##.......##.....##.##...##...##.....##.##..####.##.....##.##.......##.....##.##....##. |\n| .##....##.##.....##.##....##..##.....##.##...###.##.....##.##.......##.....##.##....##. |\n| ..######..##.....##.##.....##..#######..##....##..#######..########..#######...######.. |\n+-----------------------------------------------------------------------------------------+\n|                                Welcome to ChronoLog                                     |\n|                                                                                         |\n| ChronoLog is a cutting-edge, distributed, and tiered shared log storage ecosystem       |\n| that leverages physical time to ensure total log ordering and elastic scaling           |\n| through intelligent auto-tiering. Designed as a robust foundation for innovation,       |\n| ChronoLog supports advanced plugins including a SQL-like query engine, a streaming      |\n| processor, a log-based key-value store, and a log-based TensorFlow module.              |\n|                                                                                         |\n| Version:                                                                                |\n|   This Docker image provides a lightweight ChronoLog instance for local deployment.     |\n|   It simplifies installation and understanding of the system. For advanced              |\n|   deployment options, including production-ready configurations, please visit:          |\n|                                                                                         |\n| Useful Links:                                                                           |\n|   • Website:    https://www.chronolog.dev                                               |\n|   • Repository: https://github.com/grc-iit/ChronoLog                                    |\n|   • Wiki:       https://github.com/grc-iit/ChronoLog/wiki                               |\n|                                                                                         |\n| Thank you for choosing ChronoLog. Enjoy your session!                                   |\n+-----------------------------------------------------------------------------------------+' && exec bash"]
