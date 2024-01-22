#!/bin/bash

EXEC=/home/kfeng/CLionProjects/ChronoLog_dev/decouple_conf_manager/ChronoLog/cmake-build-release/ChronoStore/test/cmp_vlen_bytes_cpp
N_RECORDS=(100 1000 10000 100000 1000000 10000000)
MEAN_LENGTHS=(10000000 1000000 100000 10000 1000 100)
RANGE_QUERY_START_PERCENT=20
RANGE_QUERY_END_PERCENT=80
RANGE_QUERY_STEP_PERCENT=10
REP=3

for i in $(seq 1 ${REP})
do
    echo "*****************************************"
    echo "REP: ${i}"
    echo "*****************************************"
    j=0
    for j in $(seq 0 $((${#N_RECORDS[@]}-1)))
    do
        echo Testing rep "${i}" with n_records "${N_RECORDS[j]}" and mean_length "${MEAN_LENGTHS[j]}" ...
        rm -rf *.h5
        ${EXEC} "${N_RECORDS[j]}" "${MEAN_LENGTHS[j]}" ${RANGE_QUERY_START_PERCENT} ${RANGE_QUERY_END_PERCENT} ${RANGE_QUERY_STEP_PERCENT}
        sleep 5
    done
done