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
#   10 stop, clean archive, tear down
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

# Paths inside the containers -- identical to the workflow's.
WORK_DIR="/home/grc-iit/chronolog-install/chronolog"
INSTALL_DIR="/home/grc-iit/chronolog-install"
REPO_DIR="/home/grc-iit/chronolog-repo"

usage() { sed -n '3,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit "${1:-1}"; }

while [ $# -gt 0 ]; do
    case "$1" in
        -i) IMAGE="$2"; shift 2 ;;
        -k) NUM_KEEPERS="$2"; shift 2 ;;
        -g) NUM_GRAPHERS="$2"; shift 2 ;;
        -p) NUM_PLAYERS="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --keep) KEEP=1; shift ;;
        -h|--help) usage 0 ;;
        *) echo "unknown option: $1"; usage 1 ;;
    esac
done

NUM_CONTAINERS=$(( NUM_KEEPERS + NUM_GRAPHERS + NUM_PLAYERS + 1 ))

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
        echo ""
        echo "ℹ️  --keep: leaving containers up. Inspect with:"
        echo "     docker exec -it --user grc-iit chronolog-c1 bash"
        echo "   Tear down with:"
        echo "     docker compose -f ${COMPOSE_FILE} down -v"
    elif [ -f "${COMPOSE_FILE}" ]; then
        echo ""
        echo "── tearing down containers ──"
        dex c1 bash -c "cd ${REPO_DIR} 2>/dev/null && ./tools/deploy/single_user_deploy.sh -s -w ${WORK_DIR}" >/dev/null 2>&1 || true
        docker compose -f "${COMPOSE_FILE}" down -v >/dev/null 2>&1 || true
        rm -f "${COMPOSE_FILE}"
        echo "✅ containers removed"
    fi
    summary
    exit $rc
}

# `docker exec` as grc-iit into container cN. Kept short because every stage uses it.
dex() { local c="$1"; shift; docker exec --user grc-iit "chronolog-${c}" "$@"; }
dex_root() { local c="$1"; shift; docker exec --user root "chronolog-${c}" "$@"; }

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
stage_ok

trap cleanup EXIT INT TERM

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
        [ "$i" -gt 1 ] && printf "    depends_on:\n      - c1\n"
    done
    cat <<EOF

networks:
  chronolog_net:

volumes:
  shared_home_localci:
EOF
} > "${COMPOSE_FILE}"

docker compose -f "${COMPOSE_FILE}" up -d >/dev/null 2>&1 || stage_fail "compose up failed"
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
        ./tools/deploy/local_single_user_deploy.sh -b -I ${INSTALL_DIR}
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

    # The reader POLLS: an event is in the tail only between its chunk sealing and
    # ageing out, a window of about tail_retention_secs - acceptance_window_secs.
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

# --------------------------------------------------- 10. stop -----------------
stage "10. Stop ChronoLog and clean archive"
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
