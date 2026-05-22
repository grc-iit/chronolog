#!/usr/bin/env bash
set -euo pipefail
cd '/home/jcernudagarcia/chronolog-opt/chronolog'
python3 .agent/scripts/phase0_benchmark_matrix.py --systems kafka --workflows append_throughput,range_retrieval --node-counts 16 --message-sizes 65536 --operation-counts 10000 --client-counts 8 --trials 1 --partition compute --slurm-time 06:00:00 --result-dir '/home/jcernudagarcia/chronolog-opt/chronolog/.agent/results/20260522-123842-three-system-scale-baseline-4816/n16-s65536-kafka' --kafka-acks-values 0,all
