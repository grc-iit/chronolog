#!/usr/bin/env bash
# Checks the compose override for the NFS pass (#705): the volumes and
# options it prints, and the profiles it refuses.
set -uo pipefail
repo_root=${1:?repository root required}
gen="$repo_root/tests/distributed/nfs/nfs_compose_override.sh"
failures=0
check() { local m=$1; shift; if "$@"; then echo "PASS: $m"; else echo "FAIL: $m" >&2; failures=$((failures + 1)); fi; }
refuses() { ! "$gen" "$@" > /dev/null 2>&1; }

out=$("$gen" --containers 2 --server 127.0.0.1 --export /srv/x --profile default= --profile nocto=nocto,acregmin=60)
check "c1 mounts default" grep -qx '      - nfs_default_c1:/mnt/nfs/default' <<< "$out"
check "c2 mounts nocto" grep -qx '      - nfs_nocto_c2:/mnt/nfs/nocto' <<< "$out"
check "default options" grep -qx '      o: "addr=127.0.0.1,nosharecache,vers=4.2"' <<< "$out"
check "nocto options" grep -qx '      o: "addr=127.0.0.1,nosharecache,vers=4.2,nocto,acregmin=60"' <<< "$out"
check "export device" grep -qx '      device: ":/srv/x"' <<< "$out"
check "four volumes" test "$(grep -c '^  nfs_' <<< "$out")" -eq 4
out=$("$gen" --containers 1 --server 127.0.0.1 --export /srv/x --profile v3=vers=3,nocto)
check "own vers kept, no second vers" grep -qx '      o: "addr=127.0.0.1,nosharecache,vers=3,nocto"' <<< "$out"
out=$("$gen" --containers 1 --server 127.0.0.1 --export /srv/x --profile v3=nfsvers=3)
check "own nfsvers kept, no vers added" grep -qx '      o: "addr=127.0.0.1,nosharecache,nfsvers=3"' <<< "$out"
out=$("$gen" --containers 1 --server 127.0.0.1 --export /srv/x --mount-root /mnt/other --profile p=)
check "mount root" grep -qx '      - nfs_p_c1:/mnt/other/p' <<< "$out"

check "refuses no profile" refuses --containers 1 --server 127.0.0.1 --export /srv/x
check "refuses a profile without =" refuses --containers 1 --server 127.0.0.1 --export /srv/x --profile nocto
check "refuses a duplicate name" refuses --containers 1 --server 127.0.0.1 --export /srv/x --profile a= --profile a=nocto
check "refuses an upper-case name" refuses --containers 1 --server 127.0.0.1 --export /srv/x --profile Bad=
check "refuses a space in options" refuses --containers 1 --server 127.0.0.1 --export /srv/x --profile a='nocto, sync'
check "refuses a quote in options" refuses --containers 1 --server 127.0.0.1 --export /srv/x --profile 'a=nocto"'
check "refuses sharecache" refuses --containers 1 --server 127.0.0.1 --export /srv/x --profile a=sharecache
check "refuses a relative export" refuses --containers 1 --server 127.0.0.1 --export srv/x --profile a=
check "refuses zero containers" refuses --containers 0 --server 127.0.0.1 --export /srv/x --profile a=
check "refuses an unknown option" refuses --containers 1 --server 127.0.0.1 --export /srv/x --profile a= --bogus

echo "$failures NFS compose override checks failed"
test "$failures" -eq 0
