#!/usr/bin/env bash
set -euo pipefail
cd /home/jcernudagarcia/chronolog-opt/chronolog
source /etc/profile.d/lmod.sh 2>/dev/null || source /etc/profile.d/modules.sh 2>/dev/null || true
type module >/dev/null 2>&1 || { echo "module command unavailable on $(hostname)" >&2; exit 127; }
module load gcc/11.4.0/cmake/3.30.5-pq5ntgo
module load gcc/11.4.0/argobots/1.1-indjy5q
module load gcc/11.4.0/libfabric/1.22.0-lyviqpl
module load gcc/11.4.0/mercury/2.3.1-kcmn6u3
module load gcc/11.4.0/mochi-margo/0.17.0-llh62si
module load gcc/11.4.0/mochi-thallium/0.10.1-2pari3f
module load gcc/11.4.0/boost/1.86.0-cs4onpk
module load gcc/11.4.0/spdlog/1.14.1-y4quhau
module load gcc/11.4.0/json-c/0.16-2ofsb6t
module load gcc/11.4.0/pkgconf/2.2.0-3sczowe
module load gcc/11.4.0/python/3.11.9-zg4555e
module load gcc/11.4.0/py-pybind11/2.13.5-pngixx5
module load openmpi/5.0.5-x2kvx5l/gcc/11.4.0/hdf5/1.14.5-5zbyeqh
export LD_LIBRARY_PATH=/home/jcernudagarcia/chronolog-opt/chronolog/.agent/install-consistent/chronolog/lib:${LD_LIBRARY_PATH:-}
export PYTHONPATH=/home/jcernudagarcia/chronolog-opt/chronolog/.agent/install-consistent/chronolog/lib:${PYTHONPATH:-}
timeout 600s python3 /home/jcernudagarcia/chronolog-opt/chronolog/.agent/scripts/chronolog_range_retrieval.py --config /home/jcernudagarcia/chronolog-opt/chronolog/.agent/results/20260512-015243/config/default-chrono-client-conf.json --result-dir /home/jcernudagarcia/chronolog-opt/chronolog/.agent/results/20260512-015243 --operation-count 10 --message-size-bytes 1024 --node-count 2 --client-count 1
