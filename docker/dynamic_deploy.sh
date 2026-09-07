#!/bin/bash

# Function to display usage information
usage() {
    echo "Usage: $0 [-n NUM_CONTAINERS] [-k NUM_KEEPERS] [-g NUM_GRAPHERS] [-p NUM_PLAYERS] [-i IMAGE_NAME]"
    echo
    echo "Options:"
    echo "  -n NUM_CONTAINERS       Number of containers to deploy, must equal to sum of NUM_KEEPERS, NUM_GRAPHERS, and NUM_PLAYERS plus one (minimum 2, default: 4)"
    echo "  -k NUM_KEEPERS          Number of ChronoKeepers to deploy (minimum 1, default: 1)"
    echo "  -g NUM_GRAPHERS         Number of ChronoGraphers to deploy (minimum 1, default: 1)"
    echo "  -p NUM_PLAYERS          Number of ChronoPlayers to deploy (minimum 1, default: 1)"
    echo "  -i IMAGE_NAME           Docker image name to use (default: ghcr.io/grc-iit/chronolog:latest)"
    echo "  -h                      Display this help message"
    echo
    echo "Example:"
    echo "  $0 -n 4                 Deploy 4 ChronoLog containers, each for ChronoVisor, ChronoKeeper, ChronoGrapher, and ChronoPlayer"
    echo "  $0 -n 8 -k 3 -g 2 -p 2  Deploy 8 ChronoLog containers, one for ChronoVisor, three for ChronoKeeper, two for ChronoGrapher, and two for ChronoPlayer"
    exit 1
}

# Default number of containers
NUM_CONTAINERS=4
NUM_KEEPERS=1
NUM_GRAPHERS=1
NUM_PLAYERS=1
IMAGE_NAME="ghcr.io/grc-iit/chronolog:latest"

# Parse command line arguments
while getopts "n:k:g:p:i:h" opt; do
    case ${opt} in
    n)
        NUM_CONTAINERS=$OPTARG
        ;;
    k)
        NUM_KEEPERS=$OPTARG
        ;;
    g)
        NUM_GRAPHERS=$OPTARG
        ;;
    p)
        NUM_PLAYERS=$OPTARG
        ;;
    i)
        IMAGE_NAME=$OPTARG
        ;;
    h)
        usage
        ;;
    \?)
        usage
        ;;
    esac
done

# Validate number of containers
if [[ ! $NUM_CONTAINERS =~ ^[0-9]+$ ]] || [ $NUM_CONTAINERS -lt 2 ]; then
    echo "Error: Please provide a valid number of containers (minimum 2)"
    usage
fi
if [[ ! $NUM_KEEPERS =~ ^[0-9]+$ ]] || [ $NUM_KEEPERS -lt 1 ]; then
    echo "Error: Please provide a valid number of ChronoKeepers (minimum 1)"
    usage
fi
if [[ ! $NUM_GRAPHERS =~ ^[0-9]+$ ]] || [ $NUM_GRAPHERS -lt 1 ]; then
    echo "Error: Please provide a valid number of ChronoGraphers (minimum 1)"
    usage
fi
if [[ ! $NUM_PLAYERS =~ ^[0-9]+$ ]] || [ $NUM_PLAYERS -lt 1 ]; then
    echo "Error: Please provide a valid number of ChronoPlayers (minimum 1)"
    usage
fi
if [ $NUM_CONTAINERS -ne $(($NUM_KEEPERS + $NUM_GRAPHERS + $NUM_PLAYERS + 1)) ]; then
    echo "Error: Number of containers must equal to sum of NUM_KEEPERS, NUM_GRAPHERS, and NUM_PLAYERS plus one"
    usage
fi

# Generate the docker-compose file dynamically
cat >dynamic-compose.yaml <<EOF
x-common: &x-common
  image: ${IMAGE_NAME}
  init: true
  networks:
    - chronolog_net
  cap_add:
    - SYS_ADMIN
    - SYS_PTRACE
  security_opt:
    - seccomp:unconfined
    - apparmor:unconfined
  privileged: false
  mem_limit: 4g
  mem_reservation: 2g
  cpus: 2
  shm_size: 512m
  command: >
    bash -c "sudo service ssh restart && sleep infinity"

services:
EOF

# Generate the regular container services
for i in $(seq 1 $NUM_CONTAINERS); do
    cat >> dynamic-compose.yaml << EOF
  c$i:
    <<: *x-common
    hostname: c$i
    container_name: chronolog-c$i
    volumes:
      - shared_home:/home/grc-iit
    environment:
      NUM_CONTAINERS: $NUM_CONTAINERS
EOF

    # Add dependencies starting from the second container
    if [ $i -gt 1 ]; then
        cat >>dynamic-compose.yaml <<EOF
    depends_on:
      - c1
EOF
    fi
done

cat >>dynamic-compose.yaml <<EOF

networks:
  chronolog_net:

volumes:
  shared_home:
EOF

# Launch the dynamically generated docker-compose file
docker compose -f dynamic-compose.yaml up -d

# Prepare SSH keys and known hosts
docker exec chronolog-c1 bash -c "mkdir -p /home/grc-iit/.ssh && ssh-keygen -t rsa -b 4096 -f /home/grc-iit/.ssh/id_rsa -N '' || true"
docker exec chronolog-c1 bash -c "cat /home/grc-iit/.ssh/id_rsa.pub > /home/grc-iit/.ssh/authorized_keys"
docker exec chronolog-c1 bash -c "for i in \$(seq 1 $NUM_CONTAINERS); do ssh-keyscan -t rsa,ed25519 c\$i >> /home/grc-iit/.ssh/known_hosts 2>/dev/null; done"
docker exec chronolog-c1 bash -c "chmod 700 /home/grc-iit/.ssh && chmod 600 /home/grc-iit/.ssh/id_rsa && chmod 644 /home/grc-iit/.ssh/id_rsa.pub && chmod 600 /home/grc-iit/.ssh/authorized_keys"
docker exec chronolog-c1 bash -c "echo 'export USER=grc-iit' >> ~/.bashrc"
# Restart sshd in each container. This used to background a `sleep infinity` per
# container and then `wait` on them, which never returns -- everything below
# (hosts files, deployment) was unreachable. The containers are already kept
# alive by the compose `command`, so this only needs to bounce the service.
for i in $(seq 1 $NUM_CONTAINERS); do
    docker exec chronolog-c$i bash -c "sudo service ssh restart" || {
        echo "Error: could not restart ssh in chronolog-c$i"
        exit 1
    }
done

# Absolute container paths. These must NOT use `~`: the shell expands it on the
# HOST before docker exec ever sees it, producing the host user's home
# (/home/<you>/...) instead of the container's /home/grc-iit, so every path below
# pointed somewhere that does not exist inside the container.
#
# The prefix is detected rather than hardcoded: it was pinned to
# `chronolog-release-install`, but the published image installs to
# `chronolog-install`, so every hosts-file write and the deploy call landed on a
# path that does not exist -- and the script still exited 0 reporting success.
CHRONOLOG_HOME=/home/grc-iit
CHRONOLOG_PREFIX=""
for candidate in chronolog-install chronolog-release-install; do
    if docker exec chronolog-c1 test -x "${CHRONOLOG_HOME}/${candidate}/chronolog/tools/deploy_cluster.sh" 2>/dev/null; then
        CHRONOLOG_PREFIX="${CHRONOLOG_HOME}/${candidate}/chronolog"
        break
    fi
done
if [ -z "$CHRONOLOG_PREFIX" ]; then
    echo "Error: no ChronoLog install found in chronolog-c1."
    echo "       Looked for <prefix>/chronolog/tools/deploy_cluster.sh under:"
    echo "         ${CHRONOLOG_HOME}/chronolog-install"
    echo "         ${CHRONOLOG_HOME}/chronolog-release-install"
    echo "       Does image '${IMAGE_NAME}' actually contain a built ChronoLog?"
    exit 1
fi
echo "Using ChronoLog install prefix: ${CHRONOLOG_PREFIX}"
CHRONOLOG_CONF=${CHRONOLOG_PREFIX}/conf
CHRONOLOG_TOOLS=${CHRONOLOG_PREFIX}/tools

# Prepare hosts files
docker exec chronolog-c1 bash -c "rm -rf $CHRONOLOG_CONF/hosts_*"
docker exec chronolog-c1 bash -c "echo c1 > $CHRONOLOG_CONF/hosts_visor"
for i in $(seq 2 $(($NUM_KEEPERS + 1))); do
    docker exec chronolog-c1 bash -c "echo c$i >> $CHRONOLOG_CONF/hosts_keeper"
done
for i in $(seq $(($NUM_KEEPERS + 2)) $(($NUM_KEEPERS + $NUM_GRAPHERS + 1))); do
    docker exec chronolog-c1 bash -c "echo c$i >> $CHRONOLOG_CONF/hosts_grapher"
done
for i in $(seq $(($NUM_KEEPERS + $NUM_GRAPHERS + 2)) $(($NUM_KEEPERS + $NUM_GRAPHERS + $NUM_PLAYERS + 1))); do
    docker exec chronolog-c1 bash -c "echo c$i >> $CHRONOLOG_CONF/hosts_player"
done
for i in $(seq 1 $NUM_CONTAINERS); do
    docker exec chronolog-c1 bash -c "echo c$i >> $CHRONOLOG_CONF/hosts_clients"
done
for i in $(seq 1 $NUM_CONTAINERS); do
    docker exec chronolog-c1 bash -c "echo c$i >> $CHRONOLOG_CONF/hosts_all"
done

# Deploy ChronoLog (pre-built in image, no build/install needed)
if ! docker exec chronolog-c1 bash -c "$CHRONOLOG_TOOLS/deploy_cluster.sh --start"; then
    echo "Error: deploy_cluster.sh --start failed; the cluster is NOT running."
    exit 1
fi

echo "Deployed $NUM_CONTAINERS ChronoLog containers (1 for ChronoVisor, $NUM_KEEPERS for ChronoKeeper, $NUM_GRAPHERS for ChronoGrapher, and $NUM_PLAYERS for ChronoPlayer)"
