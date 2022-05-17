#!/bin/bash

CWD="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

source ${CWD}/stop_kafka.sh
source ${CWD}/stop_zookeeper.sh
