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
    echo "  -i IMAGE_NAME           Docker image name to use (default: gnosisrc/chronolog:latest)"
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
IMAGE_NAME="gnosisrc/chronolog:latest"

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
  command: >
    bash -c "sleep infinity"

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
EOF

    # Add dependencies starting from the second container
    if [ $i -gt 1 ]; then
        cat >>dynamic-compose.yaml <<EOF
    depends_on:
      - c1
EOF
    fi
done

# Add networks and volumes
cat >>dynamic-compose.yaml <<EOF

networks:
  chronolog_net:

volumes:
  shared_home:
EOF

# Launch the dynamically generated docker-compose file
docker compose -f dynamic-compose.yaml up -d

# Prepare SSH keys and known hosts (remove -it flags for CI compatibility)
# Since all containers share /home/grc-iit via shared_home volume, operations on c1
# are automatically visible in all containers - no need to copy files to each container
docker exec chronolog-c1 bash -c "mkdir -p /home/grc-iit/.ssh && ssh-keygen -t rsa -b 4096 -f /home/grc-iit/.ssh/id_rsa -N '' || true"
docker exec chronolog-c1 bash -c "cat /home/grc-iit/.ssh/id_rsa.pub > /home/grc-iit/.ssh/authorized_keys"
docker exec chronolog-c1 bash -c "for i in \$(seq 1 $NUM_CONTAINERS); do ssh-keyscan -t rsa,ed25519 c\$i >> /home/grc-iit/.ssh/known_hosts 2>/dev/null; done"
docker exec chronolog-c1 bash -c "chown -R grc-iit:grc-iit /home/grc-iit/.ssh && chmod 700 /home/grc-iit/.ssh && chmod 600 /home/grc-iit/.ssh/id_rsa && chmod 644 /home/grc-iit/.ssh/id_rsa.pub && chmod 600 /home/grc-iit/.ssh/authorized_keys"
docker exec chronolog-c1 bash -c "echo 'export USER=grc-iit' >> ~/.bashrc"

# Prepare hosts files (using absolute paths matching ci.yml)
docker exec chronolog-c1 bash -c "rm -rf /home/grc-iit/chronolog-install/chronolog/conf/hosts_*" || true
docker exec chronolog-c1 bash -c "mkdir -p /home/grc-iit/chronolog-install/chronolog/conf" || true
docker exec chronolog-c1 bash -c "echo c1 > /home/grc-iit/chronolog-install/chronolog/conf/hosts_visor"
for i in $(seq 2 $(($NUM_KEEPERS + 1))); do
    docker exec chronolog-c1 bash -c "echo c$i >> /home/grc-iit/chronolog-install/chronolog/conf/hosts_keeper"
done
for i in $(seq $(($NUM_KEEPERS + 2)) $(($NUM_KEEPERS + $NUM_GRAPHERS + 1))); do
    docker exec chronolog-c1 bash -c "echo c$i >> /home/grc-iit/chronolog-install/chronolog/conf/hosts_grapher"
done
for i in $(seq $(($NUM_KEEPERS + $NUM_GRAPHERS + 2)) $(($NUM_KEEPERS + $NUM_GRAPHERS + $NUM_PLAYERS + 1))); do
    docker exec chronolog-c1 bash -c "echo c$i >> /home/grc-iit/chronolog-install/chronolog/conf/hosts_player"
done
for i in $(seq 1 $NUM_CONTAINERS); do
    docker exec chronolog-c1 bash -c "echo c$i >> /home/grc-iit/chronolog-install/chronolog/conf/hosts_clients"
done
for i in $(seq 1 $NUM_CONTAINERS); do
    docker exec chronolog-c1 bash -c "echo c$i >> /home/grc-iit/chronolog-install/chronolog/conf/hosts_all"
done

# Note: The following build/install steps are typically done in the CI workflow
# They are included here for reference but may be skipped if using a pre-built image
# Force concretize and install dependencies in case of changes
# docker exec chronolog-c1 bash -c "cd /home/grc-iit/chronolog-repo && source /home/grc-iit/spack/share/spack/setup-env.sh && spack env activate . && spack concretize --force && spack install"

# Build ChronoLog using new build script
# docker exec chronolog-c1 bash -c "cd /home/grc-iit/chronolog-repo && source /home/grc-iit/spack/share/spack/setup-env.sh && spack env activate . && ./tools/deploy/ChronoLog/single_user_deploy.sh -b"

# Install ChronoLog using new install script
# docker exec chronolog-c1 bash -c "cd /home/grc-iit/chronolog-repo && source /home/grc-iit/spack/share/spack/setup-env.sh && spack env activate . && ./tools/deploy/ChronoLog/single_user_deploy.sh -i"

# Deploy ChronoLog using new unified work directory
# docker exec chronolog-c1 bash -c "cd /home/grc-iit/chronolog-repo && ./tools/deploy/ChronoLog/single_user_deploy.sh -d -w /home/grc-iit/chronolog-install/chronolog"

echo "Deployed $NUM_CONTAINERS ChronoLog containers (1 for ChronoVisor, $NUM_KEEPERS for ChronoKeeper, $NUM_GRAPHERS for ChronoGrapher, and $NUM_PLAYERS for ChronoPlayer)"

