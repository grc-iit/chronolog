#!/bin/bash

# Function to display usage information
usage() {
    echo "Usage: $0 [-n NUM_CONTAINERS] [-k NUM_KEEPERS] [-g NUM_GRAPHERS] [-p NUM_PLAYERS]"
    echo
    echo "Options:"
    echo "  -n NUM_CONTAINERS       Number of containers to deploy, must equal to sum of NUM_KEEPERS, NUM_GRAPHERS, and NUM_PLAYERS plus one (minimum 2, default: 4)"
    echo "  -k NUM_KEEPERS          Number of ChronoKeepers to deploy (minimum 1, default: 1)"
    echo "  -g NUM_GRAPHERS         Number of ChronoGraphers to deploy (minimum 1, default: 1)"
    echo "  -p NUM_PLAYERS          Number of ChronoPlayers to deploy (minimum 1, default: 1)"
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

# Parse command line arguments
while getopts "n:k:g:p:h" opt; do
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
cat > dynamic-compose.yaml << EOF
x-common: &x-common
  image: gnosisrc/chronolog:latest
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
    bash -c "sudo service ssh restart && sleep infinity"

services:
EOF

# Generate the regular container services
for i in $(seq 1 $(($NUM_CONTAINERS-1))); do
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
        cat >> dynamic-compose.yaml << EOF
    depends_on:
      - c1
EOF
    fi
done

# Generate the last container with setup logic
LAST_NUM=$NUM_CONTAINERS
cat >> dynamic-compose.yaml << EOF
  c$LAST_NUM:
    <<: *x-common
    hostname: c$LAST_NUM
    container_name: chronolog-c$LAST_NUM
    volumes:
      - shared_home:/home/grc-iit
    depends_on:
EOF

# Add dependencies for the last container
for i in $(seq 1 $(($NUM_CONTAINERS-1))); do
    cat >> dynamic-compose.yaml << EOF
      - c$i
EOF
done

# Add the setup command for the last container
cat >> dynamic-compose.yaml << EOF
    environment:
      NUM_CONTAINERS: $NUM_CONTAINERS
    command: >
      bash -c "
        sleep 5

        # Generate SSH keys for c$LAST_NUM (if they don't exist)
        mkdir -p /home/grc-iit/.ssh
        ssh-keygen -t rsa -b 4096 -f /home/grc-iit/.ssh/id_rsa -N '' || true;

        # Copy the public key to authorized_keys
        cat /home/grc-iit/.ssh/id_rsa.pub > /home/grc-iit/.ssh/authorized_keys;

        # Gather public keys from other containers
        for i in \$(seq 1 $NUM_CONTAINERS); do
            ssh-keyscan -t rsa,ed25519 c\$i >> /home/grc-iit/.ssh/known_hosts 2>/dev/null;
        done

        # Optional: Set appropriate permissions on .ssh directory and files
        chmod 700 /home/grc-iit/.ssh;
        chmod 600 /home/grc-iit/.ssh/id_rsa;
        chmod 644 /home/grc-iit/.ssh/id_rsa.pub;
        chmod 600 /home/grc-iit/.ssh/authorized_keys;

        # Start the SSH server (if your image doesn't start it by default)
        sudo service ssh restart && sleep infinity

        # Export env USER for old deployment scripts
        echo 'export USER=grc-iit' >> ~/.bashrc
      "

networks:
  chronolog_net:

volumes:
  shared_home:
EOF

# Launch the dynamically generated docker-compose file
docker compose -f dynamic-compose.yaml up -d

# Update Chronolog repo
docker exec -it chronolog-c1 bash -c "cd ~/chronolog_repo && git fetch origin && git reset --hard origin/pearc25 && git checkout pearc25 && git pull"

# Prepare hosts file
docker exec -it chronolog-c1 bash -c "rm -rf ~/chronolog_install/Release/conf/hosts_*"
docker exec -it chronolog-c1 bash -c "echo c1 > ~/chronolog_install/Release/conf/hosts_visor"
for i in $(seq 2 $(($NUM_KEEPERS + 1))); do
    docker exec -it chronolog-c1 bash -c "echo c$i >> ~/chronolog_install/Release/conf/hosts_keeper"
done
for i in $(seq $(($NUM_KEEPERS + 2)) $(($NUM_KEEPERS + $NUM_GRAPHERS + 1))); do
    docker exec -it chronolog-c1 bash -c "echo c$i >> ~/chronolog_install/Release/conf/hosts_grapher"
done
for i in $(seq $(($NUM_KEEPERS + $NUM_GRAPHERS + 2)) $(($NUM_KEEPERS + $NUM_GRAPHERS + $NUM_PLAYERS + 1))); do
    docker exec -it chronolog-c1 bash -c "echo c$i >> ~/chronolog_install/Release/conf/hosts_player"
done
for i in $(seq 1 $NUM_CONTAINERS); do
    docker exec -it chronolog-c1 bash -c "echo c$i >> ~/chronolog_install/Release/conf/hosts_clients"
done

# Force concretize and install dependencies in case of changes
docker exec -it chronolog-c1 bash -c "cd ~/chronolog_repo && source ~/spack/share/spack/setup-env.sh && spack env activate . && cd deploy && spack concretize --force"

# Rebuild ChronoLog
#docker exec -it chronolog-c1 bash -c "cd ~/chronolog_repo && source ~/spack/share/spack/setup-env.sh && spack env activate . && cd deploy && ./single_user_deploy.sh -b -l ~/chronolog_install"

# Reinstall ChronoLog
docker exec -it chronolog-c1 bash -c "cd ~/chronolog_repo/deploy && source ~/spack/share/spack/setup-env.sh && ./single_user_deploy.sh -i -w ~/chronolog_install/Release"

# Deploy ChronoLog
docker exec -it chronolog-c1 bash -c "cd ~/chronolog_repo/deploy && ./single_user_deploy.sh -d -w ~/chronolog_install/Release"

# Permissions for health check script
docker exec -it chronolog-c1 bash -c "chmod +x ~/chronolog_repo/test/system/health_check.sh"

echo "Deployed $NUM_CONTAINERS ChronoLog containers (1 for ChronoVisor, $NUM_KEEPERS for ChronoKeeper, $NUM_GRAPHERS for ChronoGrapher, and $NUM_PLAYERS for ChronoPlayer)"
