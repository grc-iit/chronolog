#!/bin/bash

if [[ $# -lt 1 ]]
then
  echo "Usage: $0 test_exec_bin"
  exit 1
fi

$1 &
pid=$!
if [[ -n "$pid" ]]
then
  echo PID: "$pid"
  sudo renice -n -20 -p "$pid"
  sudo cset shield --cpu 15
  cset shield -s -v
  sudo cset shield --shield --pid "$pid"
  wait "$pid"
  sudo cset shield --reset
else
  echo "cannot find $pid, something is wrong"
fi