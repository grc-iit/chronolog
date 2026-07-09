#!/bin/bash
# Build the Kafka and ChronoLog storage-backend benchmarks.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
CP=${CHRONO_PREFIX:-$HOME/chronolog-install/chronolog}
CSRC=${CHRONO_SRC:-$HOME/ChronoLog}/client/cpp/include
JSONC=$(echo $HOME/spack/opt/spack/linux-*/json-c-*/include | awk '{print $1}')

echo "[*] building kafka_bench (librdkafka $(pkg-config --modversion rdkafka))"
g++ -std=c++17 -O2 -o "$HERE/kafka_bench" "$HERE/kafka_bench.cpp" \
	$(pkg-config --cflags --libs rdkafka) -lpthread

echo "[*] building chrono_bench (ChronoLog client)"
g++ -std=c++17 -O2 -o "$HERE/chrono_bench" "$HERE/chrono_bench.cpp" \
	-I"$CP/include" -I"$CSRC" -I"$JSONC" \
	-L"$CP/lib" -lchronolog_client -Wl,-rpath,"$CP/lib" -lpthread

echo "[*] done: $HERE/kafka_bench  $HERE/chrono_bench"
