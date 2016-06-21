#!/bin/bash

#==============================================================================#
# Run parsec benchmarks:
#   - use parsecmgt + parsecperf to run each experiment
#   - on native inputs
#==============================================================================#

#set -x #echo on

#============================== PARAMETERS ====================================#
declare -a benchmarks=("blackscholes" "ferret" "swaptions" "x264" "fluidanimate" "streamcluster" "dedup")
benchinput="native"

declare -a threadsarr=(1 2 4 8 12 16)
declare -a typesarr=("native" "native_nosse" "avxswift")

action="parsecperfavx"

#========================== EXPERIMENT SCRIPT =================================#
echo "===== Results for Parsec benchmark ====="

command -v parsecmgmt >/dev/null 2>&1 || { echo >&2 "parsecmgmt is not found (did you 'source ./env.sh'?). Aborting."; exit 1; }

for times in {1..${NUM_RUNS}}; do

for bmidx in "${!benchmarks[@]}"; do
  bm="${benchmarks[$bmidx]}"

  cd ./${bm}

  # re-make the bench
  for type in "${typesarr[@]}"; do
    make ACTION=${type} clean
  done

  for threads in "${threadsarr[@]}"; do
    for type in "${typesarr[@]}"; do

      make ACTION=${type}
      echo "--- Running ${bm} ${threads} ${type} (input: ${benchinput}) ---"

      parsecmgmt -a run -p ${bm} -c clang-pthreads -s "${action}" -i ${type}-${benchinput} -n ${threads}

    done  # type
  done  # threads

  cd ../

done  # benchmarks

done  # times
