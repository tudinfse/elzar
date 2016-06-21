#!/bin/bash

#==============================================================================#
# Run parsec benchmarks:
#   - on large inputs
#   - on 16 cores
#   - collect statistics on loads/stores (0x81D0 / 0x82D0), cache-misses,
#     branches
#
# This script gets statistics for native programs
#==============================================================================#

#set -x #echo on

#============================== PARAMETERS ====================================#
declare -a benchmarks=("blackscholes" "ferret" "swaptions" "x264" "fluidanimate" "streamcluster" "dedup")
benchinput="native"

declare -a threadsarr=(16)
declare -a typesarr=("native")

action="parsecperfavxmore"

#========================== EXPERIMENT SCRIPT =================================#
echo "===== Stats for Parsec benchmark ====="

command -v parsecmgmt >/dev/null 2>&1 || { echo >&2 "parsecmgmt is not found (did you 'source ./env.sh'?). Aborting."; exit 1; }

for bmidx in "${!benchmarks[@]}"; do
  bm="${benchmarks[$bmidx]}"

  cd ./${bm}

  for threads in "${threadsarr[@]}"; do
    for type in "${typesarr[@]}"; do

        # re-make the bench
        make ACTION=${type} -B
        echo "--- Running ${bm} ${threads} ${type} (input: '${benchinput}') ---"
        parsecmgmt -a run -p ${bm} -c clang-pthreads -s "${action}" -i ${type}-${benchinput} -n ${threads}

    done  # type
  done  # threads

  cd ../

done  # benchmarks

