#!/usr/bin/env bash
set -euo pipefail

repo_root=${1:?repository root required}
SCRIPT_DIR="$repo_root/tools/deploy"
fixture=$(mktemp -d /tmp/chronolog-cleanup-test.XXXXXX)
trap 'rm -rf -- "$fixture"' EXIT
failures=0

check() {
    local message=$1
    shift
    if "$@"; then
        echo "PASS: $message"
    else
        echo "FAIL: $message" >&2
        failures=$((failures + 1))
    fi
}

publish_record() {
    mkdir -p "$1/%manifest"
    printf '%s\n' '{"op":"publish","chronicle":"C","story":"S","file":"C/S/60.1.1000.1.0.vlen.h5","start":1,"end":2}' > "$1/%manifest/1.log"
}

for script in deploy_local.sh deploy_cluster.sh; do
    # Load the production cleanup function without starting local daemons or SSH.
    source <(sed -n '/^clean_output_dir() {/,/^}/p' "$repo_root/tools/deploy/$script")
    base="$fixture/$script"
    mkdir -p "$base"

    # 'glob[1]': a glob character in the output directory's path is a literal
    for layout in direct root-link 'glob[1]'; do
        archive="$base/$layout/archive"
        mkdir -p "$archive/C/S"
        publish_record "$archive"
        touch "$archive/C/S/60.1.1000.1.0.vlen.h5" "$archive/C/S/60.1.1000.1.0.vlen.h5.partial.host.1.0"
        printf 'unrelated\n' > "$archive/C/S/notes.txt"
        cleanup_path=$archive
        if [[ "$layout" == root-link ]]; then
            cleanup_path="$base/$layout/link"
            ln -s "$archive" "$cleanup_path"
        fi
        clean_output_dir "$cleanup_path"
        check "$script $layout: published file removed" test ! -e "$archive/C/S/60.1.1000.1.0.vlen.h5"
        check "$script $layout: partial file removed" test ! -e "$archive/C/S/60.1.1000.1.0.vlen.h5.partial.host.1.0"
        check "$script $layout: unrelated file kept" test -f "$archive/C/S/notes.txt"
        check "$script $layout: manifest removed after cleanup" test ! -e "$archive/%manifest"
    done

    for layout in story-link chronicle-link internal-story-link; do
        archive="$base/$layout/archive"
        victim="$base/$layout/victim"
        mkdir -p "$archive"
        if [[ "$layout" == internal-story-link ]]; then
            victim="$archive/unrelated"
        fi
        mkdir -p "$victim"
        if [[ "$layout" == chronicle-link ]]; then
            mkdir -p "$victim/S"
            ln -s "$victim" "$archive/C"
            victim="$victim/S"
        else
            mkdir -p "$archive/C"
            ln -s "$victim" "$archive/C/S"
        fi
        publish_record "$archive"
        printf 'unrelated\n' > "$victim/60.vlen.h5"
        clean_output_dir "$archive"
        check "$script $layout: unrelated archive-shaped file kept" test -f "$victim/60.vlen.h5"
        check "$script $layout: unresolved manifest kept" test -f "$archive/%manifest/1.log"
    done
done

echo "$failures cleanup checks failed"
test "$failures" -eq 0
