#!/bin/bash

CWD="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

source ${CWD}/clean_kafka.sh
source ${CWD}/clean_zookeeper.sh
