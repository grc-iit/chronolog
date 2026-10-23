#!/usr/bin/env bash
#
# Local stand-in for .github/workflows/distributed-pipeline.yml.
#
# Runs the same multi-container pipeline the PR check runs -- same topology, same
# deploy script, same client tests, same assertions -- against your WORKING TREE,
# so a distributed regression is caught before it costs a CI round trip.
#
# Relationship to docker/dynamic_deploy.sh: that script stands up a cluster from a
# PRE-BUILT release image to try ChronoLog out. This one builds your checkout
# inside c1 the way CI does, which is what makes it useful for testing changes.
# The two generate near-identical compose files; the workflow does NOT call
# dynamic_deploy.sh, so the two have drifted and this script follows the workflow.
#
# Stages (mirroring the workflow's step names):
#   1  generate compose + launch containers
#   2  create grc-iit user, ssh, keys, hosts files
#   3  copy the working tree into c1 and build + install (shared via volume)
#   4  verify installation
#   5  deploy ChronoLog across the containers
#   6  verify deployment (process count AND host distribution)
#   7  client performance test        (chrono-bench, write path)
#   8  client read path test          (write -> tail read -> archive read)
#   9  python tail read               (writer on c2, reader on c3)
#   10 every recording group archived data
#   11 keeper shutdown delivers its retained chunks
#   11b NFS pass (only with --nfs): the archive tests again, once per mount
#       profile, with the archive on an NFS export of this host
#   11c keeper WAL kill and restart (only with --wal-restart): durable writes
#       from c2, the keeper holding them killed with SIGKILL and restarted,
#       the archive and a replay checked
#   12 stop, clean archive, tear down
#
# Usage:
#   docker/local_distributed_ci.sh [options]
#
#   -i IMAGE     base image to run containers from
#                (default: $DEFAULT_IMAGE; must contain spack + build deps)
#   -k N         keepers  (default 2, as in CI)
#   -g N         graphers (default 2, as in CI)
#   -p N         players  (default 2, as in CI)
#   --skip-build reuse whatever is already installed in the shared volume
#   --keep       leave containers running after the run (for debugging)
#   -h           this help
#
# NFS pass (#705), needs sudo on this host to export a directory:
#   --nfs                       run stage 11b after stage 11
#   --nfs-only                  as --nfs, but skip stages 7-11
#   --nfs-profile NAME=OPTIONS  an NFS mount of the export, at /mnt/nfs/NAME in
#                               every container, with these mount options
#                               (repeatable; default: "default=" and
#                               "nocto=vers=3,nocto,acregmin=60,acregmax=60")
#   --nfs-pass PROFILE:EXPECT[:warn|nowarn]
#                               one pass on PROFILE; EXPECT is clean or omission;
#                               warn/nowarn: whether the deploy must warn about
#                               the mount (repeatable; default:
#                               "default:clean:nowarn" and "nocto:omission:warn")
#   --nfs-destroy-profile NAME  profile whose clean pass also runs the destroy
#                               test, or none (default: default)
#   --nfs-export DIR            directory to export (default: /srv/chronolog-nfs-localci)
#   --nfs-cache-secs N          longest attribute cache time of the profiles
#                               (default 60; raise it with acregmax/acdirmax above 60)
#
# Keeper WAL kill and restart, kept out of CI:
#   --wal-restart               run stage 11c after stage 11 (and 11b): redeploy
#                               with the keeper WAL on a container volume at /wal
#                               in each keeper container (--wal-dir /wal), write
#                               accepted then durable events from c2, SIGKILL the
#                               keeper whose WAL holds them, restart it with
#                               --restart-keeper and check the archive
#   --wal-restart-only          as --wal-restart, but skip stages 7-11
#
# Requires: docker, docker compose. Everything else lives in the image.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE_FILE="${REPO_ROOT}/dynamic-compose.local-ci.yaml"

DEFAULT_IMAGE="ghcr.io/grc-iit/chronolog-base:latest"
IMAGE="${DEFAULT_IMAGE}"
NUM_KEEPERS=2
NUM_GRAPHERS=2
NUM_PLAYERS=2
SKIP_BUILD=0
KEEP=0
NFS=0
NFS_ONLY=0
NFS_PROFILES=()
NFS_PASSES=()
NFS_DESTROY_PROFILE=default
NFS_EXPORT=/srv/chronolog-nfs-localci
NFS_CACHE_SECS=60
NFS_OVERRIDE="${REPO_ROOT}/dynamic-compose.local-ci.nfs.yaml"
NFS_OVERRIDE_NEW=""
NFS_SERVER_UP=0
WAL_RESTART=0
WAL_RESTART_ONLY=0
WAL_CONF_PATCHED=0
CONTAINERS_CREATED=0

# Paths inside the containers -- identical to the workflow's.
WORK_DIR="/home/grc-iit/chronolog-install/chronolog"
INSTALL_DIR="/home/grc-iit/chronolog-install"
REPO_DIR="/home/grc-iit/chronolog-repo"

usage() { sed -n '3,73p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit "${1:-1}"; }

while [ $# -gt 0 ]; do
    case "$1" in
        -i) IMAGE="$2"; shift 2 ;;
        -k) NUM_KEEPERS="$2"; shift 2 ;;
        -g) NUM_GRAPHERS="$2"; shift 2 ;;
        -p) NUM_PLAYERS="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --keep) KEEP=1; shift ;;
        --nfs) NFS=1; shift ;;
        --nfs-only) NFS=1; NFS_ONLY=1; shift ;;
        --nfs-profile) NFS_PROFILES+=("$2"); shift 2 ;;
        --nfs-pass) NFS_PASSES+=("$2"); shift 2 ;;
        --nfs-destroy-profile) NFS_DESTROY_PROFILE="$2"; shift 2 ;;
        --nfs-export) NFS_EXPORT="$2"; shift 2 ;;
        --nfs-cache-secs) NFS_CACHE_SECS="$2"; shift 2 ;;
        --wal-restart) WAL_RESTART=1; shift ;;
        --wal-restart-only) WAL_RESTART=1; WAL_RESTART_ONLY=1; shift ;;
        -h|--help) usage 0 ;;
        *) echo "unknown option: $1"; usage 1 ;;
    esac
done

NUM_CONTAINERS=$(( NUM_KEEPERS + NUM_GRAPHERS + NUM_PLAYERS + 1 ))
if [ "${NFS}" = 1 ]; then
    [ ${#NFS_PROFILES[@]} -gt 0 ] || NFS_PROFILES=("default=" "nocto=vers=3,nocto,acregmin=60,acregmax=60")
    [ ${#NFS_PASSES[@]} -gt 0 ] || NFS_PASSES=("default:clean:nowarn" "nocto:omission:warn")
fi
COMPOSE_ARGS=(-f "${COMPOSE_FILE}")

# ---------------------------------------------------------------- reporting --
RUN_START=$(date +%s)
declare -a STAGE_NAMES=() STAGE_TIMES=() STAGE_RESULTS=()
CURRENT_STAGE=""
STAGE_START=0

stage() {
    CURRENT_STAGE="$1"
    STAGE_START=$(date +%s)
    echo ""
    echo "════════════════════════════════════════════════════════════════"
    echo "▶ ${CURRENT_STAGE}"
    echo "════════════════════════════════════════════════════════════════"
}

stage_ok() {
    STAGE_NAMES+=("${CURRENT_STAGE}")
    STAGE_TIMES+=("$(( $(date +%s) - STAGE_START ))")
    STAGE_RESULTS+=("pass")
    echo "✅ ${CURRENT_STAGE}"
}

# Records the failure, prints the summary, and exits. Cleanup runs via the trap.
stage_fail() {
    STAGE_NAMES+=("${CURRENT_STAGE}")
    STAGE_TIMES+=("$(( $(date +%s) - STAGE_START ))")
    STAGE_RESULTS+=("FAIL")
    echo "❌ ${CURRENT_STAGE}: ${1:-failed}"
    exit 1
}

summary() {
    local total=$(( $(date +%s) - RUN_START ))
    echo ""
    echo "════════════════════════════════════════════════════════════════"
    echo "  Local distributed pipeline summary"
    echo "════════════════════════════════════════════════════════════════"
    printf "  %-8s %-46s %s\n" "RESULT" "STAGE" "TIME"
    local i
    for i in "${!STAGE_NAMES[@]}"; do
        printf "  %-8s %-46s %ss\n" "${STAGE_RESULTS[$i]}" "${STAGE_NAMES[$i]}" "${STAGE_TIMES[$i]}"
    done
    echo "  ---------------------------------------------------------------"
    printf "  total %ss (%dm %02ds)\n" "${total}" $(( total / 60 )) $(( total % 60 ))
    echo ""
}

cleanup() {
    local rc=$?
    if [ "${KEEP}" = "1" ]; then
        # stage 11c patched the conf template of the cluster left up
        [ "${WAL_CONF_PATCHED}" = 1 ] && wal_restore_conf
        if [ "${CONTAINERS_CREATED}" = 1 ]; then
            echo ""
            echo "ℹ️  --keep: leaving containers up. Inspect with:"
            echo "     docker exec -it --user grc-iit chronolog-c1 bash"
            echo "   Tear down with:"
            echo "     docker compose ${COMPOSE_ARGS[*]} down -v"
        fi
        if [ "${NFS_SERVER_UP}" = 1 ]; then
            echo "ℹ️  --keep: leaving the NFS export. Remove it with:"
            echo "     ${REPO_ROOT}/tests/distributed/nfs/nfs_server.sh teardown --export ${NFS_EXPORT}"
        fi
    elif [ "${CONTAINERS_CREATED}" = 1 ]; then
        echo ""
        echo "── tearing down containers ──"
        dex c1 bash -c "cd ${REPO_DIR} 2>/dev/null && ./tools/deploy/single_user_deploy.sh -s -w ${WORK_DIR}" >/dev/null 2>&1 || true
        docker compose "${COMPOSE_ARGS[@]}" down -v >/dev/null 2>&1 || true
        rm -f "${COMPOSE_FILE}" "${NFS_OVERRIDE}"
        echo "✅ containers removed"
    fi
    [ -n "${NFS_OVERRIDE_NEW}" ] && rm -f "${NFS_OVERRIDE_NEW}"
    if [ "${KEEP}" != 1 ] && [ "${NFS_SERVER_UP}" = 1 ]; then
        "${REPO_ROOT}/tests/distributed/nfs/nfs_server.sh" teardown --export "${NFS_EXPORT}" >/dev/null 2>&1 || true
        echo "✅ NFS export ${NFS_EXPORT} removed"
    fi
    summary
    exit $rc
}

# `docker exec` as grc-iit into container cN. Kept short because every stage uses it.
dex() { local c="$1"; shift; docker exec --user grc-iit "chronolog-${c}" "$@"; }
dex_root() { local c="$1"; shift; docker exec --user root "chronolog-${c}" "$@"; }

# Stage 11c keeps the conf template it patches here: "single_user_deploy.sh -c"
# removes every default-chrono-conf.json.* in the conf directory.
WAL_C1_DIR=/home/grc-iit/wal-restart
WAL_CONF_TEMPLATE="${WORK_DIR}/conf/default-chrono-conf.json"
wal_restore_conf() {
    dex c1 bash -c "[ -f ${WAL_C1_DIR}/conf-template.backup ] && cp -f ${WAL_C1_DIR}/conf-template.backup ${WAL_CONF_TEMPLATE}" \
        && WAL_CONF_PATCHED=0
}

# Installed before the NFS setup so that a setup that fails halfway is torn
# down too. Containers are torn down only once this run has created them: a
# compose file left by an earlier --keep run may describe containers still in
# use.
trap cleanup EXIT INT TERM

# ------------------------------------------------------------------ preflight --
stage "Preflight"
command -v docker >/dev/null || stage_fail "docker not found"
docker compose version >/dev/null 2>&1 || stage_fail "docker compose plugin not found"
docker info >/dev/null 2>&1 || stage_fail "docker daemon not reachable"

if ! docker image inspect "${IMAGE}" >/dev/null 2>&1; then
    echo "base image ${IMAGE} not present locally, trying to pull ..."
    if ! docker pull "${IMAGE}" >/dev/null 2>&1; then
        echo ""
        echo "  The base image must contain spack and the build dependencies, because"
        echo "  this script builds your working tree inside c1 exactly as CI does."
        echo ""
        echo "  Options:"
        echo "    - pass one you already have:  -i <image>"
        echo "    - build it from this repo:    docker build -t chronolog-base:local docker/"
        echo "      (then install the spack env in it, as .github/workflows/chronolog-base.yml does)"
        echo ""
        stage_fail "base image unavailable: ${IMAGE}"
    fi
fi
echo "image     : ${IMAGE}"
echo "topology  : ${NUM_CONTAINERS} containers = 1 visor + ${NUM_KEEPERS} keeper + ${NUM_GRAPHERS} grapher + ${NUM_PLAYERS} player"
echo "repo      : ${REPO_ROOT}"
[ "${WAL_RESTART}" = 1 ] && echo "wal       : stage 11c, keeper WAL on a /wal volume in each keeper container"
if [ "${NFS}" = 1 ]; then
    echo "nfs       : export ${NFS_EXPORT}; profiles ${NFS_PROFILES[*]}; passes ${NFS_PASSES[*]}"
    echo "            exporting needs sudo"
    # check the profiles before anything is set up: the override generator
    # refuses a bad one. It goes to a file of its own until stage 1, so the
    # override of a cluster left up with --keep stays as it is.
    profile_args=()
    for p in "${NFS_PROFILES[@]}"; do profile_args+=(--profile "$p"); done
    NFS_OVERRIDE_NEW=$(mktemp "${TMPDIR:-/tmp}/chronolog-nfs-override.XXXXXX")
    "${REPO_ROOT}/tests/distributed/nfs/nfs_compose_override.sh" --containers "${NUM_CONTAINERS}" \
        --server 127.0.0.1 --export "${NFS_EXPORT}" "${profile_args[@]}" > "${NFS_OVERRIDE_NEW}" \
        || stage_fail "the NFS compose override refused the profiles"
    # set first: setup can stop after turning leases off or writing the
    # export, and teardown undoes whatever setup got to
    NFS_SERVER_UP=1
    "${REPO_ROOT}/tests/distributed/nfs/nfs_server.sh" setup --export "${NFS_EXPORT}" || stage_fail "NFS server setup failed"
fi
stage_ok

# --------------------------------------------------- 1. compose + launch ------
stage "1. Generate compose and launch containers"
{
    cat <<EOF
x-common: &x-common
  image: ${IMAGE}
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
    bash -c "sleep infinity"

services:
EOF
    for i in $(seq 1 "${NUM_CONTAINERS}"); do
        cat <<EOF
  c${i}:
    <<: *x-common
    hostname: c${i}
    container_name: chronolog-c${i}
    volumes:
      - shared_home_localci:/home/grc-iit
      - ${REPO_ROOT}:/workspace:ro
EOF
        # --wal-restart: each keeper container gets a volume of its own at
        # /wal, as a node would have a local drive, for stage 11c's WAL
        if [ "${WAL_RESTART}" = 1 ] && [ "$i" -ge 2 ] && [ "$i" -le $(( NUM_KEEPERS + 1 )) ]; then
            echo "      - wal_c${i}_localci:/wal"
        fi
        [ "$i" -gt 1 ] && printf "    depends_on:\n      - c1\n"
    done
    cat <<EOF

networks:
  chronolog_net:

volumes:
  shared_home_localci:
EOF
    if [ "${WAL_RESTART}" = 1 ]; then
        for i in $(seq 2 $(( NUM_KEEPERS + 1 ))); do echo "  wal_c${i}_localci:"; done
    fi
} > "${COMPOSE_FILE}"

if [ "${NFS}" = 1 ]; then
    mv -f "${NFS_OVERRIDE_NEW}" "${NFS_OVERRIDE}" && NFS_OVERRIDE_NEW=""
    COMPOSE_ARGS+=(-f "${NFS_OVERRIDE}")
fi

# set only now: the teardown stops and removes containers by name, and before
# compose up those can only belong to a cluster an earlier run left up
CONTAINERS_CREATED=1
if ! up_out=$(docker compose "${COMPOSE_ARGS[@]}" up -d 2>&1); then
    tail -5 <<< "${up_out}"
    stage_fail "compose up failed"
fi
sleep 10
for i in $(seq 1 "${NUM_CONTAINERS}"); do
    docker exec "chronolog-c${i}" true 2>/dev/null || stage_fail "container c${i} not responding"
done
echo "${NUM_CONTAINERS} containers up"
stage_ok

# --------------------------------------------------- 2. user / ssh / hosts ----
stage "2. Create grc-iit user, SSH and hosts files"
for i in $(seq 1 "${NUM_CONTAINERS}"); do
    dex_root "c${i}" bash -c "
        apt-get update >/dev/null 2>&1 && apt-get install -y openssh-server sudo pssh dnsutils jq chrpath >/dev/null 2>&1 || true
        if ! id -u grc-iit >/dev/null 2>&1; then
            useradd -m -s /bin/bash grc-iit
            echo 'grc-iit ALL=(ALL) NOPASSWD: ALL' >> /etc/sudoers
            mkdir -p /home/grc-iit && chown -R grc-iit:grc-iit /home/grc-iit
        fi
        service ssh start >/dev/null 2>&1 || /etc/init.d/ssh start >/dev/null 2>&1 || true
    " || stage_fail "user/ssh setup failed on c${i}"
done

dex c1 bash -c "mkdir -p /home/grc-iit/.ssh && ssh-keygen -t rsa -b 4096 -f /home/grc-iit/.ssh/id_rsa -N '' >/dev/null 2>&1 || true"
dex c1 bash -c "cat /home/grc-iit/.ssh/id_rsa.pub > /home/grc-iit/.ssh/authorized_keys"
dex c1 bash -c "for i in \$(seq 1 ${NUM_CONTAINERS}); do ssh-keyscan -t rsa,ed25519 c\$i >> /home/grc-iit/.ssh/known_hosts 2>/dev/null; done"
dex_root c1 bash -c "chown -R grc-iit:grc-iit /home/grc-iit/.ssh && chmod 700 /home/grc-iit/.ssh && chmod 600 /home/grc-iit/.ssh/id_rsa /home/grc-iit/.ssh/authorized_keys && chmod 644 /home/grc-iit/.ssh/id_rsa.pub"

# hosts files: visor on c1, then keepers, graphers, players in container order
dex_root c1 bash -c "mkdir -p ${WORK_DIR}/conf && chown -R grc-iit:grc-iit ${INSTALL_DIR}"
dex c1 bash -c "rm -f ${WORK_DIR}/conf/hosts_*"
dex c1 bash -c "echo c1 > ${WORK_DIR}/conf/hosts_visor"
for i in $(seq 2 $(( NUM_KEEPERS + 1 ))); do
    dex c1 bash -c "echo c${i} >> ${WORK_DIR}/conf/hosts_keeper"
done
for i in $(seq $(( NUM_KEEPERS + 2 )) $(( NUM_KEEPERS + NUM_GRAPHERS + 1 ))); do
    dex c1 bash -c "echo c${i} >> ${WORK_DIR}/conf/hosts_grapher"
done
for i in $(seq $(( NUM_KEEPERS + NUM_GRAPHERS + 2 )) $(( NUM_CONTAINERS ))); do
    dex c1 bash -c "echo c${i} >> ${WORK_DIR}/conf/hosts_player"
done
for i in $(seq 1 "${NUM_CONTAINERS}"); do
    dex c1 bash -c "echo c${i} >> ${WORK_DIR}/conf/hosts_clients; echo c${i} >> ${WORK_DIR}/conf/hosts_all"
done
# MPI client hosts: first min(4, N) containers, as CI does
CLIENT_HOSTS=$(( NUM_CONTAINERS < 4 ? NUM_CONTAINERS : 4 ))
dex c1 bash -c "echo c1 > ${WORK_DIR}/conf/hosts_client"
for i in $(seq 2 "${CLIENT_HOSTS}"); do
    dex c1 bash -c "echo c${i} >> ${WORK_DIR}/conf/hosts_client"
done
echo "hosts files written (client ranks: ${CLIENT_HOSTS})"
stage_ok

# --------------------------------------------------- 3. build + install -------
stage "3. Copy working tree and build + install in c1"
if [ "${SKIP_BUILD}" = "1" ]; then
    echo "--skip-build: reusing the install already in the shared volume"
    dex c1 bash -c "test -x ${WORK_DIR}/bin/chrono-visor" || stage_fail "--skip-build but no install found in the volume"
else
    dex_root c1 bash -c "mkdir -p ${REPO_DIR} && cp -r /workspace/. ${REPO_DIR}/ 2>/dev/null; chown -R grc-iit:grc-iit /home/grc-iit" \
        || stage_fail "copying the working tree into c1 failed"
    # build.sh compiles with one job per nproc, and nproc in a container
    # counts every host core, not the 2-CPU limit. On a many-core host that
    # many compilers exceed the 4 GB memory limit and get OOM-killed. GNU
    # nproc honors OMP_NUM_THREADS; 4 matches the GitHub runner.
    dex c1 bash -c "
        set -e
        cd ${REPO_DIR}
        source /home/grc-iit/spack/share/spack/setup-env.sh
        spack env activate -p .
        if [ ! -d .spack-env/view/bin ] || [ ! -d .spack-env/view/lib ]; then
            echo 'reconcretizing spack environment ...'
            spack concretize --force
        fi
        mkdir -p ${INSTALL_DIR}
        OMP_NUM_THREADS=4 ./tools/deploy/local_single_user_deploy.sh -b -I ${INSTALL_DIR}
        ./tools/deploy/local_single_user_deploy.sh -i -I ${INSTALL_DIR}
    " || stage_fail "build/install failed"
fi
stage_ok

# --------------------------------------------------- 4. verify install --------
stage "4. Verify installation"
dex c1 bash -c "
    set -e
    missing=0
    for exe in chrono-visor chrono-keeper chrono-grapher chrono-player; do
        if [ -x ${WORK_DIR}/bin/\$exe ]; then echo \"  ✅ \$exe\"; else echo \"  ❌ \$exe\"; missing=1; fi
    done
    for exe in tools/cli/chrono-client-cli tools/benchmark/chrono-bench; do
        if [ -x ${WORK_DIR}/\$exe ]; then echo \"  ✅ \$exe\"; else echo \"  ❌ \$exe\"; missing=1; fi
    done
    exit \$missing
" || stage_fail "installation verification failed"
stage_ok

# --------------------------------------------------- 5. deploy ----------------
stage "5. Deploy ChronoLog"
dex c1 bash -c "
    set -e
    cd ${REPO_DIR}
    source /home/grc-iit/spack/share/spack/setup-env.sh
    spack env activate -d . 2>/dev/null || spack env activate -p . 2>/dev/null || true
    ./tools/deploy/single_user_deploy.sh -d -w ${WORK_DIR}
" || stage_fail "deploy failed"
echo "waiting for services to come up ..."
sleep 15
stage_ok

# --------------------------------------------------- 6. verify deployment -----
stage "6. Verify deployment (counts and host distribution)"
PROCLIST=$(dex c1 bash -c "
    rm -rf /tmp/pssh_out && mkdir -p /tmp/pssh_out
    parallel-ssh -h ${WORK_DIR}/conf/hosts_all -o /tmp/pssh_out -t 30 \
      \"ps -eo pid,args 2>/dev/null | grep -E 'chrono-visor|chrono-keeper|chrono-grapher|chrono-player' | grep -v grep || true\" >/dev/null 2>&1
    for f in /tmp/pssh_out/*; do
        [ -s \"\$f\" ] && sed \"s|^|\$(basename \$f) |\" \"\$f\"
    done
    rm -rf /tmp/pssh_out
" 2>/dev/null)

check_role() {
    local proc="$1" expected_hosts="$2" expected_count="$3"
    local count; count=$(printf '%s\n' "${PROCLIST}" | grep -c "${proc}" || true)
    if [ "${count}" -ne "${expected_count}" ]; then
        echo "  ❌ ${proc}: found ${count}, expected ${expected_count}"
        return 1
    fi
    local h
    for h in ${expected_hosts}; do
        printf '%s\n' "${PROCLIST}" | grep -q "^${h} .*${proc}" || { echo "  ❌ ${proc} missing on ${h}"; return 1; }
    done
    echo "  ✅ ${proc}: ${count} on ${expected_hosts}"
    return 0
}

KEEPER_HOSTS=$(dex c1 cat "${WORK_DIR}/conf/hosts_keeper" | tr '\n' ' ')
GRAPHER_HOSTS=$(dex c1 cat "${WORK_DIR}/conf/hosts_grapher" | tr '\n' ' ')
PLAYER_HOSTS=$(dex c1 cat "${WORK_DIR}/conf/hosts_player" | tr '\n' ' ')

DEPLOY_BAD=0
check_role chrono-visor   "c1"                  1                  || DEPLOY_BAD=1
check_role chrono-keeper  "${KEEPER_HOSTS}"     "${NUM_KEEPERS}"   || DEPLOY_BAD=1
check_role chrono-grapher "${GRAPHER_HOSTS}"    "${NUM_GRAPHERS}"  || DEPLOY_BAD=1
check_role chrono-player  "${PLAYER_HOSTS}"     "${NUM_PLAYERS}"   || DEPLOY_BAD=1
if [ "${DEPLOY_BAD}" != "0" ]; then
    echo "--- processes seen ---"; printf '%s\n' "${PROCLIST}"
    stage_fail "deployment verification failed"
fi
stage_ok

# Stages 7-11 are skipped under --nfs-only and --wal-restart-only; the fi is
# before stage 11b.
if [ "${NFS_ONLY}" = 0 ] && [ "${WAL_RESTART_ONLY}" = 0 ]; then

# --------------------------------------------------- 7. perf test -------------
stage "7. Client performance test (write path)"
dex c1 bash -c "
    set -e
    cd ${REPO_DIR}
    source /home/grc-iit/spack/share/spack/setup-env.sh
    spack env activate -d . 2>/dev/null || spack env activate -p . 2>/dev/null || true
    MPIEXEC=${REPO_DIR}/.spack-env/view/bin/mpiexec
    [ -x \"\$MPIEXEC\" ] || MPIEXEC=\$(command -v mpiexec)
    [ -n \"\$MPIEXEC\" ] || { echo 'mpiexec not found'; exit 1; }
    export LD_LIBRARY_PATH=${WORK_DIR}/lib:\$LD_LIBRARY_PATH
    cd ${WORK_DIR}
    timeout 300 \$MPIEXEC -l -n ${CLIENT_HOSTS} -f conf/hosts_client \
      tools/benchmark/chrono-bench --config conf/default-chrono-client-conf.json \
      -a 4096 -b 4096 -s 4096 -n 1024 -t 1 -h 1 -p -y 2>&1 | tail -20
" || stage_fail "performance test failed"
stage_ok

# --------------------------------------------------- 8. read path -------------
stage "8. Client read path test (write -> tail read -> archive read)"
dex c1 bash -c "
    set -e
    cd ${REPO_DIR}
    source /home/grc-iit/spack/share/spack/setup-env.sh
    spack env activate -d . 2>/dev/null || spack env activate -p . 2>/dev/null || true
    MPIEXEC=${REPO_DIR}/.spack-env/view/bin/mpiexec
    [ -x \"\$MPIEXEC\" ] || MPIEXEC=\$(command -v mpiexec)
    export LD_LIBRARY_PATH=${WORK_DIR}/lib:\$LD_LIBRARY_PATH
    cd ${WORK_DIR}
    CONF=conf/default-chrono-client-conf.json
    CHRONICLE=\$(hostname); STORY=cpu_usage; CSV=/tmp/chronolog_replay.csv

    echo '--- [1/3] write: distributed telemetry writer ---'
    RC=0
    timeout 240 \$MPIEXEC -n ${CLIENT_HOSTS} -f conf/hosts_client \
      examples/chrono-client-example-distributed-telemetry-writer \
      --config \$CONF -d 30 -i 5 > /tmp/telemetry_writer.log 2>&1 || RC=\$?
    [ \$RC -eq 0 ] || { echo 'writer failed'; tail -20 /tmp/telemetry_writer.log; exit 1; }
    echo '  ✅ telemetry written'

    echo '--- [2/3] tail read: chrono-bench -l ---'
    RC=0
    timeout 300 \$MPIEXEC -n ${CLIENT_HOSTS} -f conf/hosts_client \
      tools/benchmark/chrono-bench --config \$CONF \
      -l -n 64 -t 1 -h 1 -a 4096 -s 4096 -b 4096 -e 500 -m 120 \
      > /tmp/tail_read_output.log 2>&1 || RC=\$?
    # NOTE: the workflow greps 'Latency (send' here, which matches nothing the
    # benchmark prints -- its block is headed 'Tail-read latency' with rows named
    # 'send->visible'. Using the pattern that actually matches.
    grep -E 'Tail-read latency|send->visible' /tmp/tail_read_output.log || true
    [ \$RC -eq 0 ] || { echo 'tail read failed'; tail -30 /tmp/tail_read_output.log; exit 1; }
    LINE=\$(grep -E '^Events logged: ' /tmp/tail_read_output.log | tail -1)
    echo \"\$LINE\" | grep -qE 'Events logged: [1-9][0-9]*, seen: [0-9]+, never seen \\(unsealed/evicted\\): 0\$' \
      || { echo \"  ❌ tail read gate: \${LINE:-<no Events logged line>}\"; tail -30 /tmp/tail_read_output.log; exit 1; }
    echo \"  ✅ \$LINE\"

    echo '--- [3/3] archive read: reader-to-csv ---'
    DEADLINE=\$(( SECONDS + 480 )); ROWS=0
    while [ \$SECONDS -lt \$DEADLINE ]; do
        rm -f \$CSV
        examples/chrono-client-example-reader-to-csv --config \$CONF \
          --chronicle \$CHRONICLE --story \$STORY --interval 3600 --output_file \$CSV \
          > /tmp/reader_to_csv.log 2>&1 || true
        [ -f \$CSV ] && ROWS=\$(wc -l < \$CSV) || ROWS=0
        [ \"\$ROWS\" -gt 0 ] && break
        sleep 15
    done
    [ \"\$ROWS\" -gt 0 ] || { echo '  ❌ archive read returned no rows'; ls -la ${WORK_DIR}/output | head; exit 1; }
    echo \"  ✅ archive read: \$ROWS row(s)\"
" || stage_fail "read path test failed"
stage_ok

# --------------------------------------------------- 9. python tail read ------
stage "9. Python tail read (writer on c2, reader on c3)"
if [ "${NUM_CONTAINERS}" -lt 3 ]; then
    echo "⚠️  need at least 3 containers to split writer and reader; skipping"
else
    PY_BIN="${REPO_DIR}/.spack-env/view/bin/python3"
    dex c2 test -x "${PY_BIN}" 2>/dev/null || PY_BIN=python3
    EXAMPLE="${REPO_DIR}/client/python/examples/tail_reader_client.py"
    CONF_PATH="${WORK_DIR}/conf/default-chrono-client-conf.json"

    # The writer HOLDS the story: releasing it retires the pipeline, and a retired
    # pipeline hands its chunks to the extraction queue instead of the tail.
    docker exec -d --user grc-iit chronolog-c2 bash -c "
        export PYTHONPATH=${WORK_DIR}/lib:\$PYTHONPATH
        export LD_LIBRARY_PATH=${WORK_DIR}/lib:\$LD_LIBRARY_PATH
        ${PY_BIN} -u ${EXAMPLE} --config ${CONF_PATH} --mode write \
          --chronicle py_tail_ci --story py_tail_story --events 40 --hold 300 \
          > /tmp/py_tail_writer.log 2>&1
    " || stage_fail "could not start the python writer on c2"

    # The reader POLLS: an event becomes readable only once its chunk seals
    # (story_chunk_duration_secs + acceptance_window_secs), so a single early read
    # finds nothing.
    if dex c3 bash -c "
        export PYTHONPATH=${WORK_DIR}/lib:\$PYTHONPATH
        export LD_LIBRARY_PATH=${WORK_DIR}/lib:\$LD_LIBRARY_PATH
        ${PY_BIN} -u ${EXAMPLE} --config ${CONF_PATH} --mode read \
          --chronicle py_tail_ci --story py_tail_story \
          --playback-n 10 --expect-min 1 --read-timeout 180
    "; then
        echo "  ✅ playback() on c3 returned events written by c2"
    else
        echo "--- writer log ---"
        dex c2 cat /tmp/py_tail_writer.log 2>/dev/null | tail -20 || true
        stage_fail "python tail read failed"
    fi
fi
stage_ok

# --------------------------------------------------- 10. group coverage -------
stage "10. Verify every recording group archived data"
dex c1 bash -c "
    set -e
    # HDF5FileChunkExtractor logs this only after writing a non-empty chunk. Its
    # 'processing chunk' line is also logged for empty idle-gap windows, so it is
    # not evidence that data arrived.
    MARKER='HDF5FileChunkExtractor\\] StoryChunk written to file'
    EXPECTED=\$(wc -l < ${WORK_DIR}/conf/hosts_grapher)
    LOGS=\$(ls ${WORK_DIR}/monitor/chrono-grapher*.log 2>/dev/null | grep -v '\\.launch\\.log\$' || true)
    [ -n \"\$LOGS\" ] || { echo '  ❌ no grapher logs found under monitor/'; exit 1; }
    # a grapher writes a window only after story_chunk_duration_secs plus
    # acceptance_window_secs, so poll rather than look once
    G_CONF=${WORK_DIR}/conf/default-chrono-conf.json
    G_WAIT=\$(( \$(jq -r '.chrono_grapher.DataStoreInternals.story_chunk_duration_secs' \$G_CONF 2>/dev/null || echo 60) + \$(jq -r '.chrono_grapher.DataStoreInternals.acceptance_window_secs' \$G_CONF 2>/dev/null || echo 180) + 120 ))
    DEADLINE=\$(( SECONDS + G_WAIT ))
    while :; do
        ACTIVE=0
        for f in \$LOGS; do
            if grep -q \"\$MARKER\" \"\$f\"; then ACTIVE=\$(( ACTIVE + 1 )); fi
        done
        [ \$ACTIVE -ge \$EXPECTED ] && break
        [ \$SECONDS -ge \$DEADLINE ] && break
        sleep 10
    done
    for f in \$LOGS; do
        if grep -q \"\$MARKER\" \"\$f\"; then
            echo \"  ✅ \$(basename \$f): wrote chunks to HDF5\"
        else
            echo \"  ❌ \$(basename \$f): never wrote a chunk within \${G_WAIT}s\"
        fi
    done
    echo \"  graphers that archived data: \$ACTIVE / \$EXPECTED\"
    [ \$ACTIVE -ge \$EXPECTED ]
" || stage_fail "a recording group never archived data (check each keeper conf's extractor_to_grapher.receiving_endpoint)"
stage_ok

# --------------------------------------------------- 11. keeper shutdown ------
stage "11. Keeper shutdown delivers its retained chunks"
dex c1 bash -c "
    set -e
    cd ${REPO_DIR}
    source /home/grc-iit/spack/share/spack/setup-env.sh
    spack env activate -d . 2>/dev/null || spack env activate -p . 2>/dev/null || true
    MPIEXEC=${REPO_DIR}/.spack-env/view/bin/mpiexec
    [ -x \"\$MPIEXEC\" ] || MPIEXEC=\$(command -v mpiexec)
    export LD_LIBRARY_PATH=${WORK_DIR}/lib:\$LD_LIBRARY_PATH
    cd ${WORK_DIR}

    # Write a burst and SIGTERM the keepers as soon as the writer returns: its
    # stories are still open on the keepers (their pipelines wait out the
    # acceptance window), so their last chunks seal during shutdown and must be
    # delivered on the way out. Waiting for them to seal first would leave the
    # shutdown nothing to deliver and let this stage pass without testing it.
    echo \"  writing a burst, then stopping the keepers while its last chunks are still open ...\"
    RC=0
    timeout 120 \$MPIEXEC -n 2 -f conf/hosts_client \
      examples/chrono-client-example-distributed-telemetry-writer \
      --config conf/default-chrono-client-conf.json -d 15 -i 3 > /tmp/shutdown_writer.log 2>&1 || RC=\$?
    if [ \$RC -ne 0 ]; then
        echo \"  telemetry writer failed (exit \$RC)\"
        tail -8 /tmp/shutdown_writer.log | sed 's/^/    /' || true
        exit 1
    fi

    # SIGTERM only the keepers: the graphers must stay up to receive what the
    # keepers send on the way out and to confirm it written. SIGKILL would
    # bypass shutdown entirely. A keeper waits up to shutdown_confirm_timeout_secs
    # (150) for that confirmation, so allow 240s.
    parallel-ssh -h conf/hosts_keeper -t 30 'pkill -TERM -f chrono-keeper' >/dev/null 2>&1 || true
    for i in \$(seq 1 80); do
        REMAIN=\$(parallel-ssh -h conf/hosts_keeper -i -t 10 'pgrep -c -x chrono-keeper || true' 2>/dev/null | grep -cE '^[1-9]' || true)
        [ \"\$REMAIN\" = 0 ] && break
        sleep 3
    done
    sleep 10

    # Every chunk the retention store still holds when it is destroyed is logged
    # as 'shutdown with unconfirmed'. shipped=0 never reached a grapher; shipped=1
    # reached it but was not confirmed written. Both count, as in CI. A transfer
    # logged after 'Initiating shutdown procedures' is a chunk delivered on the
    # way out; at least one keeper must have one, or the shutdown path was not
    # exercised. A log rotated past 1 MB continues in <name>.<n>.log, oldest
    # highest, so each keeper's files are read oldest first.
    EXPECTED=\$(wc -l < conf/hosts_keeper)
    LOGS=\$(ls monitor/chrono-keeper*.log 2>/dev/null | grep -v -E '\\.launch\\.log\$|\\.[0-9]+\\.log\$' || true)
    DONE=0
    LOST_TOTAL=0
    SHUT_SENT_TOTAL=0
    for f in \$LOGS; do
        FILES=\"\$(ls -r \${f%.log}.[0-9]*.log 2>/dev/null || true) \$f\"
        SENT=\$(cat \$FILES | grep -c 'ChunkExtractorRDMA\\] Transfered StoryChunk' || true)
        SHUT_SENT=\$(cat \$FILES | awk '/ChronoKeeperInstance\\] Initiating shutdown procedures/ { s = 1 } s && /ChunkExtractorRDMA\\] Transfered StoryChunk/ { n++ } END { print n + 0 }')
        LOST=\$(cat \$FILES | grep -c 'shutdown with unconfirmed' || true)
        if cat \$FILES | grep -q 'ChronoKeeperInstance\\] Shutdown completed'; then
            DONE=\$(( DONE + 1 ))
            echo \"  \$(basename \$f): shut down; \$SENT chunk(s) delivered, \$SHUT_SENT of them after SIGTERM, \$LOST unconfirmed at exit\"
        else
            echo \"  ❌ \$(basename \$f): no 'Shutdown completed'; \$SENT chunk(s) delivered, \$SHUT_SENT after SIGTERM, \$LOST unconfirmed\"
        fi
        cat \$FILES | grep -h 'Flushed .* unshipped chunk' | sed 's/^/    /' || true
        cat \$FILES | grep -h 'shutdown with unconfirmed' | head -5 | sed 's/^/    /' || true
        LOST_TOTAL=\$(( LOST_TOTAL + LOST ))
        SHUT_SENT_TOTAL=\$(( SHUT_SENT_TOTAL + SHUT_SENT ))
    done
    echo \"  keepers that completed shutdown: \$DONE / \$EXPECTED; chunks delivered after SIGTERM: \$SHUT_SENT_TOTAL; unconfirmed at exit: \$LOST_TOTAL\"
    [ \$DONE -ge \$EXPECTED ] && [ \$LOST_TOTAL -eq 0 ] && [ \$SHUT_SENT_TOTAL -gt 0 ]
" || stage_fail "the writer failed, a keeper delivered nothing on shutdown, or a chunk was never confirmed written"
stage_ok

fi # stages 7-11

# --------------------------------------------------- 11b. NFS pass ------------
if [ "${NFS}" = 1 ]; then
    stage "11b. NFS pass"
    # the scripts come from the working tree even under --skip-build
    dex_root c1 bash -c "mkdir -p ${REPO_DIR}/tests/distributed/nfs && cp -r /workspace/tests/distributed/nfs/. ${REPO_DIR}/tests/distributed/nfs/ && chown -R grc-iit:grc-iit ${REPO_DIR}/tests/distributed" \
        || stage_fail "copying the NFS pass scripts into c1 failed"
    pass_args=()
    for p in "${NFS_PASSES[@]}"; do pass_args+=(--pass "$p"); done
    if ! dex c1 bash -c "cd ${REPO_DIR} && tests/distributed/nfs/nfs_pass.sh \
        --work-dir ${WORK_DIR} --repo-dir ${REPO_DIR} \
        --probe /home/grc-iit/chronolog-build/Release/tests/integration/watermark-replay/chronolog-test-exact-split-probe \
        --destroy-profile ${NFS_DESTROY_PROFILE} --cache-secs ${NFS_CACHE_SECS} \
        --log-dir /home/grc-iit/nfs-pass-logs $(printf '%q ' "${pass_args[@]}")"; then
        # the logs are on the shared volume, which the teardown deletes
        nfs_logs=$(mktemp -d "${TMPDIR:-/tmp}/chronolog-nfs-pass-logs.XXXXXX")
        docker cp chronolog-c1:/home/grc-iit/nfs-pass-logs/. "${nfs_logs}/" > /dev/null 2>&1 || true
        stage_fail "NFS pass failed (logs copied to ${nfs_logs})"
    fi
    stage_ok
fi

# --------------------------------------------------- 11c. WAL kill and restart --
# The distributed form of tests/integration/keeper_wal_crash_test.sh. ChronoLog
# is redeployed with the keeper WAL on the /wal volume of each keeper container
# and the keeper and grapher timings that test pins. From c2, one client writes
# WAL_EVENTS accepted events to a story and releases it, then a second client
# writes WAL_EVENTS durable events to the same story. One story, not one per
# mode as in that test: the visor picks a story's recording group from its id,
# so with several groups a second story could land on a keeper that is not
# killed. The keeper whose WAL holds the story's records is killed with SIGKILL
# within WAL_KILL_WITHIN_SECS of the last durable ack, before any of the
# story's chunks can ship, and restarted with --restart-keeper, which reuses
# its conf and WAL directory. Checks:
#   1. the kill came within WAL_KILL_WITHIN_SECS of the last durable ack and
#      less than WAL_K_ACCEPT_SECS after the first accepted event, and the
#      killed keeper left no exit-summary;
#   2. the restarted keeper recovers its WAL: the archive holds at least
#      WAL_EVENTS events of the story, and the segment that held the durable
#      records stays until it does;
#   3. after the grapher confirmed them, that segment is deleted and the WAL
#      directory holds at most two *.wal files;
#   4. a replay from another container returns every acknowledged durable
#      identity exactly once;
#   5. negative control: the same replay misses at least one acknowledged
#      accepted event;
#   6. single_user_deploy.sh -s finds every keeper's exit-summary clean.
# With more than one keeper in the story's group, the other keepers' accepted
# events reach the archive too, so check 2 alone shows less; checks 4 and 5
# still hold.
if [ "${WAL_RESTART}" = 1 ]; then
    stage "11c. Keeper WAL kill and restart"
    WAL_PROBE=/home/grc-iit/chronolog-build/Release/tests/integration/keeper-wal/chronolog-test-durable-probe
    WAL_H5DUMP="${REPO_DIR}/.spack-env/view/bin/h5dump"
    WAL_CLIENT_CONF="${WORK_DIR}/conf/default-chrono-client-conf.json"
    WAL_CHRONICLE=KeeperWalRestartChronicle
    WAL_STORY=WalRestartStory
    WAL_EVENTS=200
    WAL_KILL_WITHIN_SECS=2
    # as in keeper_wal_crash_test.sh: keeper chunks ship at chunk end plus the
    # acceptance window, so a kill less than WAL_K_ACCEPT_SECS after the first
    # event comes before any of them shipped; small grapher windows write the
    # archive soon after the restart
    WAL_K_CHUNK_SECS=10
    WAL_K_ACCEPT_SECS=15
    WAL_G_CHUNK_SECS=10
    WAL_G_ACCEPT_SECS=15
    WAL_ARCHIVE_DEADLINE=240
    WAL_DEADLINE=120
    WAL_WRITER=c2
    WAL_READER="c${NUM_CONTAINERS}"
    WAL_LOGS=$(mktemp -d "${TMPDIR:-/tmp}/chronolog-wal-restart-logs.XXXXXX")
    WAL_PASS=0
    WAL_FAIL=0

    wal_ok() { echo "  ✅ PASS: $*"; WAL_PASS=$(( WAL_PASS + 1 )); }
    wal_bad() { echo "  ❌ FAIL: $*"; WAL_FAIL=$(( WAL_FAIL + 1 )); }
    now_ns() { date +%s%N; }

    # single_user_deploy.sh in c1 with the spack env, as stage 5 runs it
    wal_deploy() {
        dex c1 bash -c "cd ${REPO_DIR} && source /home/grc-iit/spack/share/spack/setup-env.sh && \
            { spack env activate -d . 2>/dev/null || spack env activate -p . 2>/dev/null || true; } && \
            ./tools/deploy/single_user_deploy.sh $(printf '%q ' "$@")"
    }
    wal_kill_all() {
        local i
        for i in $(seq 1 "${NUM_CONTAINERS}"); do
            dex "c${i}" pkill -9 -f "chrono-(visor|keeper|grapher|player) --config" > /dev/null 2>&1
        done
        sleep 2
    }
    # wal_probe HOST CLIENT_CONF ARGS...: the durable probe on HOST, on the story
    wal_probe() {
        local host=$1 conf=$2
        shift 2
        dex "${host}" env "LD_LIBRARY_PATH=${WORK_DIR}/lib" "${WAL_PROBE}" --config "${WAL_C1_DIR}/${conf}" \
            --chronicle "${WAL_CHRONICLE}" --story "${WAL_STORY}" "$@"
    }
    wal_keeper_pid() { dex "$1" pgrep -f "chrono-keeper --config" 2> /dev/null | head -1; }
    wal_segments() { dex "${WAL_KILL_HOST}" find "${WAL_KILL_DIR}" -maxdepth 1 -name '*.wal' 2> /dev/null | wc -l; }
    # the oldest segment of keeper host $1's WAL directory $2 whose N record names the chronicle
    wal_held_segment() {
        dex "$1" bash -c "grep -lF -- '${WAL_CHRONICLE}' '$2'/*.wal 2>/dev/null | sort | head -1 | xargs -r basename"
    }
    wal_held_present() { [ -z "${WAL_HELD}" ] || dex "${WAL_KILL_HOST}" test -e "${WAL_KILL_DIR}/${WAL_HELD}"; }
    # events of the story across its archive files
    wal_archived() {
        docker exec -i --user grc-iit chronolog-c1 bash -s -- "${WAL_H5DUMP}" \
            "${WORK_DIR}/output/${WAL_CHRONICLE}/${WAL_STORY}" <<'EOF'
total=0
for f in "$2"/*.h5; do
    [ -f "$f" ] || continue
    n=$("$1" -H -d /story_chunks/data.vlen_bytes "$f" 2> /dev/null |
        sed -n 's/.*DATASPACE *SIMPLE *{ *( *\([0-9]*\) *).*/\1/p' | head -1)
    total=$((total + ${n:-0}))
done
echo "$total"
EOF
    }
    identities() { grep -E '^[0-9]+ [0-9]+ [0-9]+$' "$1" | sort -u; }

    # wal_write MODE: WAL_EVENTS events from the writer; acknowledged identities to $WAL_LOGS/acked_MODE
    wal_write() {
        local out="${WAL_LOGS}/write_$1.out"
        wal_probe "${WAL_WRITER}" writer_client_conf.json --write "${WAL_EVENTS}" --mode "$1" > "${out}" 2>&1
        identities "${out}" > "${WAL_LOGS}/acked_$1"
        echo "  $1 writes from ${WAL_WRITER}: $(sed -n 's/^WRITE_ACKED //p' "${out}") acked/sent; codes: $(sed -n 's/^WRITE_CODE //p' "${out}" | paste -sd ',' -)"
        [ "$(wc -l < "${WAL_LOGS}/acked_$1")" -eq "${WAL_EVENTS}" ]
    }

    wal_restart_pass() {
        dex c1 test -x "${WAL_PROBE}" || { wal_bad "probe not found in c1: ${WAL_PROBE}"; return 1; }
        dex c1 grep -qa 'durable|accepted' "${WAL_PROBE}" ||
            { wal_bad "${WAL_PROBE} has no durable mode: it was built from older sources; rebuild it"; return 1; }
        dex c1 test -x "${WAL_H5DUMP}" || { wal_bad "h5dump not found in c1: ${WAL_H5DUMP}"; return 1; }

        echo "  stopping the running deployment and cleaning it"
        wal_deploy -s -w "${WORK_DIR}" > "${WAL_LOGS}/stop-before.log" 2>&1
        wal_kill_all
        wal_deploy -c -w "${WORK_DIR}" > "${WAL_LOGS}/clean-before.log" 2>&1
        # a WAL left in a volume would be recovered into this run
        local h
        for h in ${KEEPER_HOSTS}; do
            dex_root "${h}" bash -c "find /wal -mindepth 1 -maxdepth 1 -exec rm -rf {} + && chown grc-iit:grc-iit /wal" ||
                { wal_bad "cannot prepare /wal in ${h}"; return 1; }
        done

        echo "  patching the conf template: keeper ${WAL_K_CHUNK_SECS}s chunks / ${WAL_K_ACCEPT_SECS}s window, grapher ${WAL_G_CHUNK_SECS}s / ${WAL_G_ACCEPT_SECS}s"
        dex c1 bash -c "mkdir -p ${WAL_C1_DIR} && cp -f ${WAL_CONF_TEMPLATE} ${WAL_C1_DIR}/conf-template.backup" ||
            { wal_bad "cannot back up ${WAL_CONF_TEMPLATE}"; return 1; }
        WAL_CONF_PATCHED=1
        dex c1 bash -c "jq '.chrono_keeper.DataStoreInternals.story_chunk_duration_secs = ${WAL_K_CHUNK_SECS} |
            .chrono_keeper.DataStoreInternals.acceptance_window_secs = ${WAL_K_ACCEPT_SECS} |
            .chrono_keeper.DataStoreInternals.archive_visibility_delay_secs = 3 |
            .chrono_keeper.DataStoreInternals.shutdown_confirm_timeout_secs = 30 |
            .chrono_grapher.DataStoreInternals.story_chunk_duration_secs = ${WAL_G_CHUNK_SECS} |
            .chrono_grapher.DataStoreInternals.acceptance_window_secs = ${WAL_G_ACCEPT_SECS} |
            .chrono_grapher.DataStoreInternals.watermark_report_interval_secs = 1' \
            ${WAL_C1_DIR}/conf-template.backup > ${WAL_C1_DIR}/conf-template.patched &&
            cp -f ${WAL_C1_DIR}/conf-template.patched ${WAL_CONF_TEMPLATE}" ||
            { wal_bad "conf template patch failed"; return 1; }

        echo "  deploying with the keeper WAL in /wal"
        if ! wal_deploy -d -w "${WORK_DIR}" --wal-dir /wal > "${WAL_LOGS}/deploy.log" 2>&1; then
            tail -20 "${WAL_LOGS}/deploy.log"
            wal_bad "deploy with --wal-dir /wal failed"
            return 1
        fi
        grep -E 'WAL directory|WARNING' "${WAL_LOGS}/deploy.log" | sed 's/\x1b\[[0-9;]*m//g; s/^/    /'
        sleep 15

        # host -> WAL directory, from the keeper confs the deploy generated
        declare -gA WAL_DIR_OF=()
        local d
        while read -r h d; do
            [ -n "${h}" ] && WAL_DIR_OF["${h}"]="${d}"
        done < <(dex c1 bash -c "for c in ${WAL_CONF_TEMPLATE}.*.keeper.*; do [ -f \"\$c\" ] && echo \"\${c##*.keeper.} \$(jq -r '.chrono_keeper.WAL.dir' \"\$c\")\"; done")
        for h in ${KEEPER_HOSTS}; do
            if [ -z "$(wal_keeper_pid "${h}")" ]; then
                wal_bad "deploy: no keeper running on ${h}"
                return 1
            fi
            case "${WAL_DIR_OF[${h}]:-}" in
                /wal/*) ;;
                *) wal_bad "deploy: keeper conf for ${h} has WAL dir '${WAL_DIR_OF[${h}]:-}', not under /wal"; return 1 ;;
            esac
        done

        # the writer and the reader bind their query services on ports of their own
        dex c1 bash -c "jq '.chrono_client.ClientQueryService.rpc.service_base_port = 5564' ${WAL_CLIENT_CONF} > ${WAL_C1_DIR}/writer_client_conf.json &&
            jq '.chrono_client.ClientQueryService.rpc.service_base_port = 5563' ${WAL_CLIENT_CONF} > ${WAL_C1_DIR}/reader_client_conf.json" ||
            { wal_bad "client conf patch failed"; return 1; }

        # ------------------------------------------------ write, then kill --
        wal_write accepted || { wal_bad "accepted writes: not every event was acknowledged"; return 1; }
        wal_write durable || { wal_bad "durable writes: not every event was acknowledged"; return 1; }
        local last_ack_ns first_accepted_ns
        last_ack_ns=$(sed -n 's/^LAST_ACK_NS //p' "${WAL_LOGS}/write_durable.out")
        first_accepted_ns=$(cut -d' ' -f1 "${WAL_LOGS}/acked_accepted" | sort -n | head -1)

        # the keeper whose WAL holds the story's records; with one keeper, no search
        WAL_KILL_HOST=""
        read -ra keeper_list <<< "${KEEPER_HOSTS}"
        if [ "${#keeper_list[@]}" -eq 1 ]; then
            WAL_KILL_HOST=${keeper_list[0]}
        else
            for h in "${keeper_list[@]}"; do
                [ -n "$(wal_held_segment "${h}" "${WAL_DIR_OF[${h}]}")" ] && { WAL_KILL_HOST=${h}; break; }
            done
        fi
        [ -n "${WAL_KILL_HOST}" ] || { wal_bad "kill: no keeper WAL names ${WAL_CHRONICLE}"; return 1; }
        WAL_KILL_DIR=${WAL_DIR_OF[${WAL_KILL_HOST}]}
        local pid kill_ns gap_ms span_ms
        pid=$(wal_keeper_pid "${WAL_KILL_HOST}")
        [ -n "${pid}" ] || { wal_bad "kill: no keeper running on ${WAL_KILL_HOST} after the writes"; return 1; }
        dex "${WAL_KILL_HOST}" bash -c "kill -9 ${pid}"
        kill_ns=$(now_ns)
        gap_ms=$(( (kill_ns - last_ack_ns) / 1000000 ))
        span_ms=$(( (kill_ns - first_accepted_ns) / 1000000 ))
        local i
        for i in $(seq 1 50); do [ -z "$(wal_keeper_pid "${WAL_KILL_HOST}")" ] && break; sleep 0.1; done
        if [ -n "$(wal_keeper_pid "${WAL_KILL_HOST}")" ]; then
            wal_bad "kill: keeper ${pid} on ${WAL_KILL_HOST} still running after SIGKILL"
            return 1
        elif [ "${gap_ms}" -lt $(( WAL_KILL_WITHIN_SECS * 1000 )) ]; then
            wal_ok "kill: keeper ${pid} on ${WAL_KILL_HOST} killed ${gap_ms} ms after the last durable ack"
        else
            wal_bad "kill: keeper on ${WAL_KILL_HOST} killed ${gap_ms} ms after the last durable ack, over ${WAL_KILL_WITHIN_SECS}s"
        fi
        if [ "${span_ms}" -lt $(( WAL_K_ACCEPT_SECS * 1000 )) ]; then
            wal_ok "kill: ${span_ms} ms after the first accepted event, under the ${WAL_K_ACCEPT_SECS}s acceptance window"
        else
            wal_bad "kill: ${span_ms} ms after the first accepted event, not under the ${WAL_K_ACCEPT_SECS}s acceptance window; chunks may have shipped"
        fi
        WAL_HELD=$(wal_held_segment "${WAL_KILL_HOST}" "${WAL_KILL_DIR}")
        if dex "${WAL_KILL_HOST}" test -e "${WAL_KILL_DIR}/exit-summary"; then
            wal_bad "kill: the killed keeper left ${WAL_KILL_DIR}/exit-summary, as only a clean exit should"
        else
            wal_ok "kill: no exit-summary in ${WAL_KILL_HOST}:${WAL_KILL_DIR}; $(wal_segments) *.wal file(s), records in ${WAL_HELD:-none}"
        fi

        # ---------------------------------------------------------- restart --
        echo "  restarting the keeper on ${WAL_KILL_HOST}"
        if ! wal_deploy --restart-keeper "${WAL_KILL_HOST}" -w "${WORK_DIR}" > "${WAL_LOGS}/restart.log" 2>&1; then
            cat "${WAL_LOGS}/restart.log"
            wal_bad "restart: single_user_deploy.sh --restart-keeper ${WAL_KILL_HOST} failed"
            return 1
        fi
        local restart_ns archived early_release=0 deadline
        restart_ns=$(now_ns)
        echo "  waiting up to ${WAL_ARCHIVE_DEADLINE}s for ${WAL_EVENTS} events of ${WAL_STORY} in the archive"
        # the segment is looked at before the archive is counted, so a segment
        # gone while the count is short was deleted before the archive held them
        deadline=$(( SECONDS + WAL_ARCHIVE_DEADLINE ))
        wal_held_present || early_release=1
        archived=$(wal_archived)
        while [ "${archived:-0}" -lt "${WAL_EVENTS}" ] && [ "${early_release}" -eq 0 ] && [ "${SECONDS}" -lt "${deadline}" ]; do
            [ -n "$(wal_keeper_pid "${WAL_KILL_HOST}")" ] || break
            sleep 5
            wal_held_present || early_release=1
            archived=$(wal_archived)
        done
        if [ "${early_release}" -eq 1 ] && [ "${archived:-0}" -lt "${WAL_EVENTS}" ]; then
            wal_bad "recovery: ${WAL_HELD} deleted while only ${archived} of ${WAL_EVENTS} event(s) were in the archive"
        elif [ -z "$(wal_keeper_pid "${WAL_KILL_HOST}")" ]; then
            wal_bad "recovery: the restarted keeper on ${WAL_KILL_HOST} exited"
            return 1
        elif [ "${archived:-0}" -ge "${WAL_EVENTS}" ]; then
            wal_ok "recovery: ${archived} event(s) of ${WAL_STORY} in the archive $(( ($(now_ns) - restart_ns) / 1000000000 ))s after the restart"
        else
            wal_bad "recovery: only ${archived:-0} of ${WAL_EVENTS} event(s) in the archive after ${WAL_ARCHIVE_DEADLINE}s"
        fi

        echo "  waiting up to ${WAL_DEADLINE}s for ${WAL_HELD:-the held segment} to go and the WAL to drop to at most 2 segments"
        deadline=$(( SECONDS + WAL_DEADLINE ))
        while { wal_held_present || [ "$(wal_segments)" -gt 2 ]; } && [ "${SECONDS}" -lt "${deadline}" ]; do
            sleep 2
        done
        local segments
        segments=$(wal_segments)
        if [ -z "${WAL_HELD}" ]; then
            wal_bad "WAL: no *.wal file in ${WAL_KILL_DIR} names ${WAL_CHRONICLE} after the kill"
        elif wal_held_present; then
            wal_bad "WAL: ${WAL_HELD}, which held the durable records, still in ${WAL_KILL_DIR} after ${WAL_DEADLINE}s"
        elif [ "${segments}" -le 2 ]; then
            wal_ok "WAL: ${WAL_HELD} deleted after confirmation; ${segments} *.wal file(s) left in ${WAL_KILL_DIR}"
        else
            wal_bad "WAL: still ${segments} *.wal file(s) in ${WAL_KILL_DIR} after ${WAL_DEADLINE}s"
        fi

        # ------------------------------------------------------------ replay --
        wal_probe "${WAL_READER}" reader_client_conf.json --replay > "${WAL_LOGS}/replay.out" 2>&1
        local status missing twice unexpected lost
        status=$(sed -n 's/^REPLAY_STATUS \(.*\)$/\1/p' "${WAL_LOGS}/replay.out")
        grep -E '^[0-9]+ [0-9]+ [0-9]+$' "${WAL_LOGS}/replay.out" > "${WAL_LOGS}/replay.ids"
        echo "  replay from ${WAL_READER}: status ${status:-?}, $(wc -l < "${WAL_LOGS}/replay.ids") event(s)"
        missing=$(comm -23 "${WAL_LOGS}/acked_durable" <(sort -u "${WAL_LOGS}/replay.ids") | wc -l)
        # an identity returned twice, or a second event at the same (time,
        # client id) under another index, is a second copy of an acked event
        twice=$(awk 'NR == FNR { acked[$1 " " $2] = 1; next } ($1 " " $2) in acked { n[$1 " " $2]++ }
                     END { c = 0; for(k in n) if(n[k] > 1) c++; print c }' \
                    "${WAL_LOGS}/acked_durable" "${WAL_LOGS}/replay.ids")
        unexpected=$(comm -13 <(sort -u "${WAL_LOGS}/acked_durable" "${WAL_LOGS}/acked_accepted") \
                         <(sort -u "${WAL_LOGS}/replay.ids") | wc -l)
        lost=$(comm -23 "${WAL_LOGS}/acked_accepted" <(sort -u "${WAL_LOGS}/replay.ids") | wc -l)
        if [ "${status}" != "CL_SUCCESS" ]; then
            wal_bad "replay: status ${status:-none}"
            grep -Ev '^[0-9]+ [0-9]+ [0-9]+$' "${WAL_LOGS}/replay.out" | tail -5 | sed 's/^/    /'
        else
            if [ "${missing}" -eq 0 ] && [ "${twice}" -eq 0 ]; then
                wal_ok "durable replay: every one of the ${WAL_EVENTS} acknowledged durable identities appears exactly once (${unexpected} unacknowledged)"
            else
                wal_bad "durable replay: ${missing} acknowledged durable identities missing, ${twice} returned more than once"
            fi
            if [ "${lost}" -ge 1 ]; then
                wal_ok "negative control: ${lost} of ${WAL_EVENTS} acknowledged accepted events lost in the kill"
            else
                wal_bad "negative control: every acknowledged accepted event survived the kill; the kill did not test the WAL"
            fi
        fi

        # -------------------------------------------------------------- stop --
        echo "  stopping the deployment"
        if wal_deploy -s -w "${WORK_DIR}" > "${WAL_LOGS}/stop.log" 2>&1; then
            wal_ok "stop: every keeper's exit-summary is clean"
        else
            wal_bad "stop: single_user_deploy.sh -s reported a missing exit-summary or unconfirmed durable events"
        fi
        grep -A "${NUM_KEEPERS}" 'PENDING_EVENTS' "${WAL_LOGS}/stop.log" | sed 's/\x1b\[[0-9;]*m//g; s/^/    /'
        dex c1 bash -c "cat \$(ls -r ${WORK_DIR}/monitor/chrono-keeper-${WAL_KILL_HOST}.[0-9]*.log 2>/dev/null) ${WORK_DIR}/monitor/chrono-keeper-${WAL_KILL_HOST}.log 2>/dev/null | grep 'WAL replay from' | tail -1" |
            sed 's/^/    /'
        return 0
    }

    wal_restart_pass
    wal_kill_all
    wal_restore_conf || echo "  could not restore ${WAL_CONF_TEMPLATE} from ${WAL_C1_DIR}/conf-template.backup"
    echo "  ${WAL_PASS} passed, ${WAL_FAIL} failed"
    if [ "${WAL_FAIL}" -ne 0 ] || [ "${WAL_PASS}" -eq 0 ]; then
        # the daemon logs are on the shared volume, which the teardown deletes
        mkdir -p "${WAL_LOGS}/monitor"
        docker cp chronolog-c1:"${WORK_DIR}/monitor/." "${WAL_LOGS}/monitor/" > /dev/null 2>&1 || true
        stage_fail "keeper WAL kill and restart failed (logs in ${WAL_LOGS})"
    fi
    rm -rf "${WAL_LOGS}"
    stage_ok
fi

# --------------------------------------------------- 12. stop -----------------
stage "12. Stop ChronoLog and clean archive"
dex c1 bash -c "
    cd ${REPO_DIR} 2>/dev/null || true
    source /home/grc-iit/spack/share/spack/setup-env.sh 2>/dev/null || true
    spack env activate -d . 2>/dev/null || true
    ./tools/deploy/single_user_deploy.sh -s -w ${WORK_DIR} 2>/dev/null || true
    find ${WORK_DIR}/output -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2>/dev/null || true
    rm -f /tmp/chronolog_replay.csv /tmp/*_output.log /tmp/telemetry_writer.log \
          /tmp/reader_to_csv.log /tmp/py_tail_*.log 2>/dev/null || true
" >/dev/null 2>&1 || true
stage_ok

echo ""
echo "🎉 Local distributed pipeline passed."
