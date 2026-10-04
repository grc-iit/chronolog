# Sourced by run.sh. Removes the images a smoke run superseded, for one engine at a time.
#
# smoke_images_snapshot ENGINE REF... records the image id each REF names before the run builds it.
# smoke_images_cleanup ENGINE REF... then removes an image only when it is dangling, no container
# (running or stopped) uses it, and it belongs to one of the REFs: either its id was recorded by the
# snapshot, or (Podman, whose images keep NamesHistory) its most recent name was one of the REFs, which
# also catches what earlier runs of the same tag left behind. Images of other tags, base images,
# intermediate layers the current images still use and the Docker BuildKit cache are never touched.
# The engine is whatever "docker" or "podman" resolves to on PATH, so a test can substitute a fake one.

declare -gA smoke_image_snapshot=()

smoke_engine() {
    local engine=$1
    shift
    if [ "$engine" = docker ]; then
        DOCKER_HOST=${SMOKE_DOCKER_HOST:-unix:///run/user/1000/docker.sock} timeout 60 docker "$@"
    else
        timeout 60 podman "$@"
    fi
}

# Podman names images built by podman build localhost/NAME:TAG and images built through the compose
# provider docker.io/library/NAME:TAG; Docker reports the bare NAME:TAG.
smoke_ref_matches() {
    local name=$1 ref
    shift
    for ref in "$@"; do
        case "$name" in
            "$ref" | "localhost/$ref" | "docker.io/library/$ref") return 0 ;;
        esac
    done
    return 1
}

smoke_images_snapshot() {
    local engine=$1 ref id ids=""
    shift
    for ref in "$@"; do
        id=$(smoke_engine "$engine" image inspect --format '{{.Id}}' "$ref" 2> /dev/null) || continue
        ids+=" ${id#sha256:}"
    done
    smoke_image_snapshot[$engine]="${smoke_image_snapshot[$engine]:-}$ids"
}

smoke_images_cleanup() {
    local engine=$1
    shift
    local candidates=() id line names first used=" " removed=0 bytes=0 size kept=0
    for id in ${smoke_image_snapshot[$engine]:-}; do
        candidates+=("$id")
    done
    if [ "$engine" = podman ]; then
        while read -r id; do
            [ -n "$id" ] || continue
            line=$(smoke_engine podman image inspect --format '{{range .NamesHistory}}{{.}} {{end}}' "$id" 2> /dev/null < /dev/null) || continue
            first=${line%% *}
            if [ -n "$first" ] && smoke_ref_matches "$first" "$@"; then
                candidates+=("${id#sha256:}")
            fi
        done < <(smoke_engine podman images --filter dangling=true --no-trunc --format '{{.ID}}' 2> /dev/null)
    fi
    if [ "${#candidates[@]}" -eq 0 ]; then
        echo "-- $engine: image cleanup found nothing to remove"
        return 0
    fi
    local containers
    containers=$(smoke_engine "$engine" ps -aq --no-trunc 2> /dev/null) || {
        echo "-- $engine: image cleanup skipped, cannot list containers"
        return 0
    }
    if [ -n "$containers" ]; then
        while read -r id; do
            used+="${id#sha256:} "
        done < <(smoke_engine "$engine" container inspect --format '{{.Image}}' $containers 2> /dev/null)
    fi
    local seen=" " root free_before free_after
    if [ "$engine" = docker ]; then
        root=$(smoke_engine docker info --format '{{.DockerRootDir}}' 2> /dev/null)
    else
        root=$(smoke_engine podman info --format '{{.Store.GraphRoot}}' 2> /dev/null)
    fi
    free_before=$(df --output=avail -B1 "${root:-/}" 2> /dev/null | tail -1)
    for id in "${candidates[@]}"; do
        case "$seen" in *" $id "*) continue ;; esac
        seen+="$id "
        names=$(smoke_engine "$engine" image inspect --format '{{len .RepoTags}} {{.Size}}' "$id" 2> /dev/null) || continue
        size=${names#* }
        if [ "${names%% *}" != 0 ]; then
            continue
        fi
        case "$used" in *" $id "*)
            kept=$((kept + 1))
            continue
            ;;
        esac
        if smoke_engine "$engine" rmi "$id" > /dev/null 2>&1; then
            removed=$((removed + 1))
            bytes=$((bytes + size))
        fi
    done
    free_after=$(df --output=avail -B1 "${root:-/}" 2> /dev/null | tail -1)
    echo "-- $engine: image cleanup removed $removed superseded image(s) of this run's tag" \
        "($((bytes / 1000000)) MB of image data, storage free space change $(((${free_after:-0} - ${free_before:-0}) / 1000000)) MB);" \
        "kept $kept in use by a container"
}
