#!/bin/bash

#==============================================================================#
# Run Phoenix benchmarks:
#   - on large inputs
#   - on 16 cores
#   - collect statistics on loads/stores (0x81D0 / 0x82D0), cache-misses,
#     branches
#
# This script gets statistics for native programs
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
declare -a typesarr=("native")

action="perf stat -r 20 -e cycles,instructions -e cache-references,cache-misses -e L1-dcache-loads,L1-dcache-load-misses,L1-dcache-stores,L1-dcache-store-misses -e r81D0,r82D0 -e branches,branch-misses -e r7C6"

#========================== EXPERIMENT SCRIPT =================================#
echo "===== Stats for Phoenix benchmark ====="

for bmidx in "${!benchmarks[@]}"; do
  bm="${benchmarks[$bmidx]}"
  in="${benchinputs[$bmidx]}"

  cd ./${bm}

  # dry run to load files into RAM
  echo "--- Dry run for ${bm} (input '${in}') ---"
  make ACTION=${type} -B
  ./build/native/${bm} ${in}

  for threads in "${threadsarr[@]}"; do
    for type in "${typesarr[@]}"; do

      echo "--- Running ${bm} ${threads} ${type} (input: '${in}') ---"
      # physical cores start with 0, so be compliant
      realthreads=$((threads-1))
      ${action} taskset -c 0-${realthreads} ./build/${type}/${bm} ${in}

    done  # type
  done  # threads

  cd ../
done # benchmarks

