#!/usr/bin/env bash
# Runs image_cleanup.sh against fake docker and podman commands and asserts that only the run's own
# dangling and superseded images are removed. Run from the repository root: bash tests/smoke/image_cleanup_test.sh
set -uo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin"
export FAKE_ENGINE_STATE=$work

# State per engine: images lines "id|tag tag|history history|size", containers lines "container|image id".
cat > "$work/bin/podman" << 'FAKE'
#!/usr/bin/env bash
engine=$(basename "$0")
images=$FAKE_ENGINE_STATE/$engine.images
containers=$FAKE_ENGINE_STATE/$engine.containers
prefix=""
[ "$engine" = docker ] && prefix=sha256:
find_image() {
    local key=${1#sha256:} id tags history size
    while IFS='|' read -r id tags history size; do
        if [ "$id" = "$key" ] || [[ " $tags " == *" $1 "* || " $tags " == *" localhost/$1 "* || " $tags " == *" docker.io/library/$1 "* ]]; then
            echo "$id|$tags|$history|$size"
            return 0
        fi
    done < "$images"
    return 1
}
case "$1 $2" in
    "image inspect")
        format=$4
        record=$(find_image "$5") || { echo "no such image $5" >&2; exit 1; }
        IFS='|' read -r id tags history size <<< "$record"
        case "$format" in
            '{{.Id}}') echo "$prefix$id" ;;
            '{{range .NamesHistory}}{{.}} {{end}}') [ "$engine" = podman ] || exit 1; echo "${history:+$history }" ;;
            '{{len .RepoTags}} {{.Size}}') set -- $tags; echo "$# $size" ;;
            *) echo "unexpected format $format" >&2; exit 1 ;;
        esac
        ;;
    "images --filter")
        [ "$3 $4 $5 $6" = "dangling=true --no-trunc --format {{.ID}}" ] || exit 1
        while IFS='|' read -r id tags history size; do
            [ -z "$tags" ] && echo "sha256:$id"
        done < "$images"
        ;;
    "ps -aq")
        cut -d'|' -f1 "$containers"
        ;;
    "container inspect")
        shift 4
        for container in "$@"; do
            grep "^$container|" "$containers" | cut -d'|' -f2 | sed "s/^/$prefix/"
        done
        ;;
    "info --format")
        echo "$FAKE_ENGINE_STATE"
        ;;
    rmi*)
        key=${2#sha256:}
        grep -q "|$key\$" "$containers" && { echo "image in use" >&2; exit 1; }
        grep -q "^$key|" "$images" || exit 1
        grep -v "^$key|" "$images" > "$images.new"
        mv "$images.new" "$images"
        echo "$engine $key" >> "$FAKE_ENGINE_STATE/removed"
        ;;
    *) echo "unexpected call: $*" >&2; exit 1 ;;
esac
FAKE
chmod +x "$work/bin/podman"
ln -s podman "$work/bin/docker"
export PATH=$work/bin:$PATH
: > "$work/removed"

refs=(chronolog-runtime-local:wt-test chronolog-viz:wt-test chronolog-mcp:wt-test)

cat > "$work/podman.images" << 'STATE'
old1|localhost/chronolog-runtime-local:wt-test|localhost/chronolog-runtime-local:wt-test|2000000000
viz1|docker.io/library/chronolog-viz:wt-test|docker.io/library/chronolog-viz:wt-test|800000000
prev||docker.io/library/chronolog-mcp:wt-test localhost/chronolog-mcp:wt-other|900000000
othertree||localhost/chronolog-runtime-local:wt-other|2000000000
similar||localhost/chronolog-viz:wt-test2|800000000
foreignrepo||quay.io/someone/chronolog-viz:wt-test|800000000
older||localhost/chronolog-runtime-local:wt-other localhost/chronolog-runtime-local:wt-test|2000000000
inuse||localhost/chronolog-mcp:wt-test|900000000
nohistory|||100000000
base|docker.io/library/ubuntu:24.04|docker.io/library/ubuntu:24.04|80000000
current-other|localhost/chronolog-runtime-local:wt-other|localhost/chronolog-runtime-local:wt-other|2000000000
STATE
echo "c1|inuse" > "$work/podman.containers"
cat > "$work/docker.images" << 'STATE'
dold|chronolog-runtime-local:wt-test||3000000000
dviz|chronolog-viz:wt-test||1000000000
dother|||3000000000
dused|||3000000000
dbase|grafana/grafana:11.6.0||800000000
STATE
echo "c9|dused" > "$work/docker.containers"

source "$root/tests/smoke/image_cleanup.sh"
smoke_images_snapshot podman "${refs[@]}"
smoke_images_snapshot docker "${refs[@]}"

# The run rebuilds the runtime image under both engines, so old1 and dold lose their tag. The viz images
# keep theirs, so their recorded ids must survive. dused stands for a recorded id a container still uses.
sed -i 's/^old1|[^|]*|/old1||/' "$work/podman.images"
echo "new1|localhost/chronolog-runtime-local:wt-test|localhost/chronolog-runtime-local:wt-test|2000000000" >> "$work/podman.images"
sed -i 's/^dold|[^|]*|/dold||/' "$work/docker.images"
echo "dnew|chronolog-runtime-local:wt-test||3000000000" >> "$work/docker.images"
smoke_image_snapshot[docker]+=" dused"

smoke_images_cleanup podman "${refs[@]}"
smoke_images_cleanup docker "${refs[@]}"

expected=$'docker dold\npodman old1\npodman prev'
actual=$(sort "$work/removed")
if [ "$actual" != "$expected" ]; then
    echo "FAIL image cleanup removed:"
    echo "$actual"
    echo "expected:"
    echo "$expected"
    exit 1
fi
for kept in viz1 othertree similar foreignrepo older inuse nohistory base current-other new1; do
    grep -q "^$kept|" "$work/podman.images" || { echo "FAIL podman image $kept was removed"; exit 1; }
done
for kept in dviz dother dused dbase dnew; do
    grep -q "^$kept|" "$work/docker.images" || { echo "FAIL docker image $kept was removed"; exit 1; }
done
echo "PASS image cleanup removes only this run's superseded images"
