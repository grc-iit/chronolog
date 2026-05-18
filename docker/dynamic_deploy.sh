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

# Build ordered list of all hostnames: visor first, then keepers, graphers, players
ALL_HOSTS=(visor)
for i in $(seq 1 $NUM_KEEPERS);  do ALL_HOSTS+=(keeper-$i);  done
for i in $(seq 1 $NUM_GRAPHERS); do ALL_HOSTS+=(grapher-$i); done
for i in $(seq 1 $NUM_PLAYERS);  do ALL_HOSTS+=(player-$i);  done

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
  visor:
    <<: *x-common
    hostname: visor
    container_name: chronolog-visor
    volumes:
      - shared_home:/home/grc-iit
    environment:
      NUM_CONTAINERS: $NUM_CONTAINERS

EOF

for i in $(seq 1 $NUM_KEEPERS); do
  cat >>dynamic-compose.yaml <<EOF
  keeper-$i:
    <<: *x-common
    hostname: keeper-$i
    container_name: chronolog-keeper-$i
    volumes:
      - shared_home:/home/grc-iit
    depends_on:
      - visor

EOF
done

for i in $(seq 1 $NUM_GRAPHERS); do
  cat >>dynamic-compose.yaml <<EOF
  grapher-$i:
    <<: *x-common
    hostname: grapher-$i
    container_name: chronolog-grapher-$i
    volumes:
      - shared_home:/home/grc-iit
    depends_on:
      - visor

EOF
done

for i in $(seq 1 $NUM_PLAYERS); do
  cat >>dynamic-compose.yaml <<EOF
  player-$i:
    <<: *x-common
    hostname: player-$i
    container_name: chronolog-player-$i
    volumes:
      - shared_home:/home/grc-iit
    depends_on:
      - visor

EOF
done

cat >>dynamic-compose.yaml <<EOF
networks:
  chronolog_net:

volumes:
  shared_home:
EOF

# Launch the dynamically generated docker-compose file
docker compose -f dynamic-compose.yaml up -d

# Prepare SSH keys and known hosts on visor
docker exec -it chronolog-visor bash -c "mkdir -p /home/grc-iit/.ssh && ssh-keygen -t rsa -b 4096 -f /home/grc-iit/.ssh/id_rsa -N '' || true"
docker exec -it chronolog-visor bash -c "cat /home/grc-iit/.ssh/id_rsa.pub > /home/grc-iit/.ssh/authorized_keys"
docker exec -it chronolog-visor bash -c "for host in ${ALL_HOSTS[*]}; do ssh-keyscan -t rsa,ed25519 \$host >> /home/grc-iit/.ssh/known_hosts 2>/dev/null; done"
docker exec -it chronolog-visor bash -c "chmod 700 /home/grc-iit/.ssh && chmod 600 /home/grc-iit/.ssh/id_rsa && chmod 644 /home/grc-iit/.ssh/id_rsa.pub && chmod 600 /home/grc-iit/.ssh/authorized_keys"
docker exec -it chronolog-visor bash -c "echo 'export USER=grc-iit' >> ~/.bashrc"
for host in "${ALL_HOSTS[@]}"; do
  docker exec -it chronolog-$host bash -c "sudo service ssh restart && sleep infinity" &
done
wait

docker exec -it chronolog-visor bash -c "mv ~/chronolog-install ~/chronolog_install"

# Update Chronolog repo
docker exec -it chronolog-visor bash -c "cd ~/chronolog_repo && git reset --hard origin/develop && git pull"

# Prepare hosts files
docker exec -it chronolog-visor bash -c "rm -rf ~/chronolog_install/chronolog/conf/hosts_*"
docker exec -it chronolog-visor bash -c "echo visor > ~/chronolog_install/chronolog/conf/hosts_visor"
for i in $(seq 1 $NUM_KEEPERS); do
  docker exec -it chronolog-visor bash -c "echo keeper-$i >> ~/chronolog_install/chronolog/conf/hosts_keeper"
done
for i in $(seq 1 $NUM_GRAPHERS); do
  docker exec -it chronolog-visor bash -c "echo grapher-$i >> ~/chronolog_install/chronolog/conf/hosts_grapher"
done
for i in $(seq 1 $NUM_PLAYERS); do
  docker exec -it chronolog-visor bash -c "echo player-$i >> ~/chronolog_install/chronolog/conf/hosts_player"
done
for host in "${ALL_HOSTS[@]}"; do
  docker exec -it chronolog-visor bash -c "echo $host >> ~/chronolog_install/chronolog/conf/hosts_clients && echo $host >> ~/chronolog_install/chronolog/conf/hosts_all"
done

# Force concretize and install dependencies in case of changes
docker exec -it chronolog-visor bash -c "cd ~/chronolog_repo && source ~/spack/share/spack/setup-env.sh && spack env activate . && spack concretize --force && spack install"

# Build ChronoLog using new build script
docker exec -it chronolog-visor bash -c "cd ~/chronolog_repo && source ~/spack/share/spack/setup-env.sh && spack env activate . && ./tools/deploy/ChronoLog/single_user_deploy.sh -b"

# Install ChronoLog using new install script
docker exec -it chronolog-visor bash -c "cd ~/chronolog_repo && source ~/spack/share/spack/setup-env.sh && spack env activate . && ./tools/deploy/ChronoLog/single_user_deploy.sh -i"

# Deploy ChronoLog using new unified work directory
docker exec -it chronolog-visor bash -c "cd ~/chronolog_repo && ./tools/deploy/ChronoLog/single_user_deploy.sh -d -w ~/chronolog_install/chronolog"

echo "Deployed $NUM_CONTAINERS ChronoLog containers (1 for ChronoVisor, $NUM_KEEPERS for ChronoKeeper, $NUM_GRAPHERS for ChronoGrapher, and $NUM_PLAYERS for ChronoPlayer)"
