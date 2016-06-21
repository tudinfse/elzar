#!/bin/bash

#==============================================================================#
# Run parsec benchmarks:
#   - use parsecmgt + parsecperf to run each experiment
#   - on native inputs
#
# This script tests performance of benchmarks with different checks en/disabled
#==============================================================================#

#set -x #echo on

#============================== PARAMETERS ====================================#
declare -a benchmarks=("blackscholes" "ferret" "swaptions" "x264" "fluidanimate" "streamcluster" "dedup")
benchinput="native"

declare -a threadsarr=(16)
declare -a typesarr=("avxswift")
declare -a flagsarr=(\
"" \
"-no-check-load" \
"-no-check-load -no-check-store" \
"-no-check-load -no-check-store -no-check-branch" \
"-no-check-load -no-check-store -no-check-branch -no-check-call" \
"-no-check-all" \
)

action="parsecperfavx"

#========================== EXPERIMENT SCRIPT =================================#
echo "===== Results for Parsec benchmark ====="

command -v parsecmgmt >/dev/null 2>&1 || { echo >&2 "parsecmgmt is not found (did you 'source ./env.sh'?). Aborting."; exit 1; }

for times in {1..${NUM_RUNS}}; do

for bmidx in "${!benchmarks[@]}"; do
  bm="${benchmarks[$bmidx]}"

  cd ./${bm}

  for threads in "${threadsarr[@]}"; do
    for type in "${typesarr[@]}"; do
      for flags in "${flagsarr[@]}"; do

        # re-make the bench
        make ACTION=${type} clean
        make ACTION=${type} SIMDSWIFT_PASSFLAGS="${flags}"

        echo "--- Running ${bm} ${threads} ${type} (input: '${benchinput}') (flags: '${flags}') ---"

        parsecmgmt -a run -p ${bm} -c clang-pthreads -s "${action}" -i ${type}-${benchinput} -n ${threads}

      done  # flags
    done  # type
  done  # threads

  cd ../

done  # benchmarks

done  # times
