# Functions shared by the NFS pass scripts (#705). Source it; it runs nothing.

# run_on HOST CMD...: runs CMD on HOST over ssh; HOST "local" runs it here.
run_on() {
    local host=$1
    shift
    if [ "$host" = local ]; then
        bash -c "$*"
    else
        ssh -n -o BatchMode=yes "$host" "$*"
    fi
}

# probe_on HOST ARGS...: runs the exact-split probe on HOST. The caller sets
# PROBE, CLIENT_CONF and WORK_DIR.
probe_on() {
    local host=$1
    shift
    run_on "$host" "LD_LIBRARY_PATH='$WORK_DIR/lib' '$PROBE' --config '$CLIENT_CONF' $*"
}

# conf_value JQ_PATH DEFAULT: a value from the deployed conf template, or
# DEFAULT when the template lacks it.
conf_value() {
    jq -r "$1 // empty" "$WORK_DIR/conf/default-chrono-conf.json" 2> /dev/null | grep . || echo "$2"
}

# classify_replay EXPECTED_FILE OUTPUT_FILE: compares a probe replay with the
# timestamps that were written (one per line in EXPECTED_FILE) and prints
# "<CLASS> missing=<n> extra=<n> dup=<n> status=<status>". CLASS, first match
# wins: ERROR (no REPLAY_STATUS line, or a failure other than a partial
# result), PARTIAL, UNEXPECTED (an event that was not written), DUPLICATE,
# OMISSION (CL_SUCCESS without every event), OK. Refuses, with status 2, an
# EXPECTED_FILE that is missing or empty: against nothing, every replay
# would look complete.
classify_replay() {
    local expected=$1 output=$2
    if [ ! -s "$expected" ]; then
        echo "ERROR missing=0 extra=0 dup=0 status=empty-expected"
        return 2
    fi
    local status got missing extra dup class
    status=$(awk '$1 == "REPLAY_STATUS" { s = $2 } END { print s }' "$output")
    got=$(awk '$1 == "EVENT" { print $2 }' "$output" | sort)
    dup=$(uniq -d <<< "$got" | grep -c . || true)
    missing=$(comm -23 <(sort -u "$expected") <(sort -u <<< "$got") | grep -c . || true)
    extra=$(comm -13 <(sort -u "$expected") <(sort -u <<< "$got") | grep -c . || true)
    if [ -z "$status" ]; then
        class=ERROR status=none
    elif [ "$status" = CL_ERR_PARTIAL_RESULT ]; then
        class=PARTIAL
    elif [ "$status" != CL_SUCCESS ]; then
        class=ERROR
    elif [ "$extra" -gt 0 ]; then
        class=UNEXPECTED
    elif [ "$dup" -gt 0 ]; then
        class=DUPLICATE
    elif [ "$missing" -gt 0 ]; then
        class=OMISSION
    else
        class=OK
    fi
    echo "$class missing=$missing extra=$extra dup=$dup status=$status"
}

# timeline_matches EXPECT TIMELINE: returns 0 when the visibility test's
# timeline, one classify_replay line per replay, shows what EXPECT asks for,
# 1 when it does not, 2 for an unknown EXPECT.
#   clean     at least one replay, and every replay OK
#   omission  at least one OMISSION, the last replay OK, and no replay with an
#             event that was not written or with an event twice, whatever its
#             status: a partial result is classified PARTIAL before its extra
#             and dup counts. The last replay is past the cache time, so a
#             stale cache has expired by then and only lost events stay missing.
timeline_matches() {
    local expect=$1 timeline=$2 n ok omission corrupt last
    n=$(grep -c . "$timeline" || true)
    ok=$(grep -c ' OK ' "$timeline" || true)
    omission=$(grep -c ' OMISSION ' "$timeline" || true)
    corrupt=$(grep -cE ' (extra|dup)=[1-9]' "$timeline" || true)
    last=$(grep . "$timeline" | tail -n 1)
    case "$expect" in
        clean) [ "$n" -gt 0 ] && [ "$ok" -eq "$n" ] ;;
        omission) [ "$omission" -gt 0 ] && [ "$corrupt" -eq 0 ] && [[ "$last" == *' OK '* ]] ;;
        *) return 2 ;;
    esac
}
