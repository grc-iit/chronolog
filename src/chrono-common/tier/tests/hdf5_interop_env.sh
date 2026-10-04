#!/usr/bin/env bash
# Prepares build/h5py-venv, the interpreter grapher.HDF5Interop reads archives with. Idempotent: a prepared venv
# costs one import. A failure here fails the interop test; it is never a skip.
set -euo pipefail
venv=$1
mkdir -p "$(dirname "$venv")"
exec 9> "$venv.lock"
flock 9
if [ -x "$venv/bin/python" ] && "$venv/bin/python" -c 'import h5py' 2>/dev/null; then
    exit 0
fi
python3 -m venv "$venv"
"$venv/bin/pip" install --quiet 'h5py>=3.11,<4'
"$venv/bin/python" -c 'import h5py; print("h5py", h5py.__version__, "HDF5", h5py.version.hdf5_version)'
