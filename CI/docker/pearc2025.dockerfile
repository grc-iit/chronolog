FROM ubuntu:22.04

SHELL ["/bin/bash", "-c"]

ARG USERNAME=grc-iit
ARG UID=1001

# Set noninteractive frontend
ENV DEBIAN_FRONTEND=noninteractive
ENV DEBCONF_NOWARNINGS=yes
ENV TZ=America/Chicago

# First install the missing Perl module to prevent QEMU segfault
#RUN apt-get update && apt-get install -y --no-install-recommends \
#    libterm-readline-perl-perl \
#    perl-modules \
#    && rm -rf /var/lib/apt/lists/*

# Install system dependencies
RUN apt-get update \
# && apt-mark hold libc6 libc-bin libc6-dev \
# && apt-get -y upgrade \
 && DEBIAN_FRONTEND=noninteractive apt-get -y install \
    gcc g++ automake cmake libtool pkgconf \
    libjson-c-dev libboost-dev libcereal-dev \
    git vim fuse sudo curl wget \
    libfuse-dev libssl-dev \
    python3-dev bzip2 xz-utils jq net-tools lsof htop pssh bind9-dnsutils \
    apt-transport-https software-properties-common \
    adduser libfontconfig1 musl

RUN apt-get -y install --no-install-recommends \
    openssh-server \
  && printf '%s\n' \
    'PermitRootLogin yes' \
    'PasswordAuthentication yes' \
    'PermitEmptyPasswords yes' \
    'UsePAM no' \
    > /etc/ssh/sshd_config.d/auth.conf \
  && printf '%s\n' \
    'Host *' \
    '    StrictHostKeyChecking no' \
    > /etc/ssh/ssh_config.d/ignore-host-key.conf

RUN id $UID && userdel $(id -un $UID) || : \
 && useradd -m -u $UID -s /bin/bash $USERNAME \
 && echo "$USERNAME ALL=(ALL:ALL) NOPASSWD: ALL" >> /etc/sudoers.d/$USERNAME \
 && passwd -d $USERNAME

# Switch to that user
USER $USERNAME 

# Set the working directory to the user's home directory
WORKDIR /home/$USERNAME

# Get ChronoLog
RUN cd \
 && git clone https://github.com/grc-iit/ChronoLog.git chronolog_repo\
 && cd chronolog_repo \
 && git switch pearc25 \
 && git pull

# Install dependencies and ChronoLog, then clean Spack
RUN cd \
 && git clone --branch v0.21.2 https://github.com/spack/spack.git \
 && export SPACK_ROOT=$(pwd)/spack \
 && source spack/share/spack/setup-env.sh \
 && cd chronolog_repo \
 && spack env activate . \
 && spack install -v \
 && mkdir -p build \
 && cd build \
 && export USER=$(whoami) \
 && cmake -DCMAKE_BUILD_TYPE=Release -DINSTALL_DIR=/home/$USERNAME/chronolog_install .. \
 && make -j \
 && cd ../deploy \
 # && ./local_single_user_deploy.sh -b --install-dir ~/chronolog_install/Release \
 && ./local_single_user_deploy.sh -i --work-dir ~/chronolog_install/Release

# Install mpssh for distributed deployment
RUN cd \
 && git clone https://github.com/ndenev/mpssh.git \
 && cd mpssh \
 && make \
 && sudo make install \
 && cd \
 && rm -rf mpssh

# Source Spack on each shell
RUN cd \
 && printf '%s\n' \
    'source ~/spack/share/spack/setup-env.sh' \
    >> .bashrc

# Export USER=grc-iit on each shell
RUN cd \
 && printf '%s\n' \
    'export USER=grc-iit' \
    >> .bashrc

# Install Grafana
RUN cd \
 && wget https://dl.grafana.com/enterprise/release/grafana-enterprise_12.0.2+security~01_amd64.deb \
 && sudo dpkg -i ./grafana-enterprise_12.0.2+security~01_amd64.deb \
 && rm ./grafana-enterprise_12.0.2+security~01_amd64.deb

# Welcome message
CMD ["/bin/bash", "-c", "echo $'+-----------------------------------------------------------------------------------------+\n| ..######..##.....##.########...#######..##....##..#######..##........#######...######.. |\n| .##....##.##.....##.##.....##.##.....##.###...##.##.....##.##.......##.....##.##....##. |\n| .##.......##.....##.##.....##.##.....##.####..##.##.....##.##.......##.....##.##....... |\n| .##.......#########.########..##.....##.##.##.##.##.....##.##.......##.....##.##...#### |\n| .##.......##.....##.##...##...##.....##.##..####.##.....##.##.......##.....##.##....##. |\n| .##....##.##.....##.##....##..##.....##.##...###.##.....##.##.......##.....##.##....##. |\n| ..######..##.....##.##.....##..#######..##....##..#######..########..#######...######.. |\n+-----------------------------------------------------------------------------------------+\n|                                Welcome to ChronoLog                                     |\n|                                                                                         |\n| ChronoLog is a cutting-edge, distributed, and tiered shared log storage ecosystem       |\n| that leverages physical time to ensure total log ordering and elastic scaling           |\n| through intelligent auto-tiering. Designed as a robust foundation for innovation,       |\n| ChronoLog supports advanced plugins including a SQL-like query engine, a streaming      |\n| processor, a log-based key-value store, and a log-based TensorFlow module.              |\n|                                                                                         |\n| Version:                                                                                |\n|   This Docker image provides a lightweight ChronoLog instance for local deployment.     |\n|   It simplifies installation and understanding of the system. For advanced              |\n|   deployment options, including production-ready configurations, please visit:          |\n|                                                                                         |\n| Useful Links:                                                                           |\n|   • Website:    https://www.chronolog.dev                                               |\n|   • Repository: https://github.com/grc-iit/ChronoLog                                    |\n|   • Wiki:       https://github.com/grc-iit/ChronoLog/wiki                               |\n|                                                                                         |\n| Thank you for choosing ChronoLog. Enjoy your session!                                   |\n+-----------------------------------------------------------------------------------------+' && exec bash"]
