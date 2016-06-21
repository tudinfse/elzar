#!/bin/bash

#==============================================================================#
# Run Phoenix benchmarks:
#   - on large inputs
#   - use taskset -c 0-.. to limit number of CPUs
#
# This script tests performance of benchmarks with different checks en/disabled
#==============================================================================#

#set -x #echo on

#============================== PARAMETERS ====================================#
declare -a benchmarks=( \
"histogram" \
"kmeans" \
"linear_regression" \
"matrix_multiply" \
"pca" \
"string_match" \
"word_count" \
)

declare -a benchinputs=(\
"input/large.bmp"\                 # histogram
" "\                               # kmeans -- dont need anything
"input/key_file_500MB.txt"\        # linear
"1500"\                            # matrix multiply (requires created files)
"-r 3000 -c 3000"\                 # pca
"input/key_file_500MB.txt"\        # string match
"input/word_100MB.txt"\            # word count
)

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

#action="/usr/bin/time -p"
action="perf stat -e cycles,instructions -e branches,branch-misses -e r7C6"

#========================== EXPERIMENT SCRIPT =================================#
echo "===== Results for Phoenix benchmark ====="

for times in {1..${NUM_RUNS}}; do


sudo sh -c 'echo 3 >/proc/sys/vm/drop_caches'

for bmidx in "${!benchmarks[@]}"; do
  bm="${benchmarks[$bmidx]}"
  in="${benchinputs[$bmidx]}"

  cd ./${bm}

  # dry run to load files into RAM
  echo "--- Dry run for ${bm} (input '${in}') ---"
  make ACTION=native
  ./build/native/${bm} ${in}

  for threads in "${threadsarr[@]}"; do
    for type in "${typesarr[@]}"; do
      for flags in "${flagsarr[@]}"; do

        # remake the bench
        make ACTION=${type} clean
        make ACTION=${type} SIMDSWIFT_PASSFLAGS="${flags}"

        echo "--- Running ${bm} ${threads} ${type} (input: '${in}') (flags: '${flags}') ---"

        # physical cores start with 0, so be compliant
        realthreads=$((threads-1))
        ${action} taskset -c 0-${realthreads} ./build/${type}/${bm} ${in}

      done  # flags
    done  # type
  done  # threads

  cd ../
done # benchmarks

done # times
