#!/usr/bin/env bash

# Removes what a deployment wrote into its output directory, and nothing else,
# so an output directory shared with other data (-o /tmp, say) keeps it: the
# archive files in each story directory the manifest's publish records name,
# then that directory and its chronicle directory if that left them empty,
# then the manifest. The output directory may be a symlink; a story directory
# that resolves anywhere but <output>/<chronicle>/<story> is skipped. The
# manifest stays when jq could not read it or a story directory was skipped or
# could not be emptied, so a later clean finds the rest through it. Each step
# goes on when one before it fails, under set -e too.
#
# Usage: archive_cleanup.sh <output dir>
clean_output_dir() {
    local dir=$1
    [[ -d "${dir}" ]] || return 0
    local archive_root
    archive_root=$(realpath -e -- "${dir}") || return 0
    local archive_files=(\( -name '*.vlen.h5' -o -name '*.vlen.*.h5' -o -name '*.h5.partial.*' \))
    local story story_path missed=0 records
    # find, not a glob: the output directory's path may hold glob characters
    if [[ -n "$(find "${dir}/%manifest" -maxdepth 1 -type f -name '*.log' -print -quit 2> /dev/null)" ]]; then
        records=$(mktemp)
        if ! command -v jq > /dev/null ||
            ! jq -R -j 'fromjson? | select(.op == "publish" and (.file | type) == "string")
                | (.file | split("/") | .[:-1] | join("/")) + "\u0000"' "${dir}/%manifest/"*.log > "${records}"; then
            echo "Could not read the archive manifest in ${dir}/%manifest with jq" >&2
            missed=1
        fi
        while IFS= read -r -d '' story; do
            # a record names <chronicle>/<story>/<file>; never anything above the archive
            [[ -z "${story}" || "${story}" == /* || "/${story}/" == *"/../"* ]] && continue
            [[ -d "${dir}/${story}" ]] || continue
            if ! story_path=$(realpath -e -- "${dir}/${story}") ||
                [[ "${story_path}" != "${archive_root%/}/${story}" ]]; then
                echo "Kept ${dir}/${story}: cleanup does not follow symlinks below the archive root" >&2
                missed=1
                continue
            fi
            find -P "${story_path}" -maxdepth 1 -type f "${archive_files[@]}" -delete || missed=1
            rmdir "${story_path}" 2> /dev/null || true
            rmdir "$(dirname "${story_path}")" 2> /dev/null || true
        done < <(sort -zu "${records}")
        rm -f "${records}"
    fi
    if [[ ${missed} -eq 0 ]]; then
        rm -rf "${dir}/%manifest" || echo "Could not remove ${dir}/%manifest" >&2
    else
        echo "Kept ${dir}/%manifest: some archive files could not be removed, and a later clean finds them through it" >&2
    fi
    return 0
}

if [[ "${BASH_SOURCE[0]:-}" == "$0" ]]; then
    clean_output_dir "${1:?archive directory required}"
fi
