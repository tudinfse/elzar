#!/bin/bash

#==============================================================================#
# Run Phoenix benchmarks:
#   - on large inputs
#   - use taskset -c 0-.. to limit number of CPUs
#   - comparison with Swift-R
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
declare -a typesarr=("native" "swiftr" "avxswift")

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

  # remake the bench
  for type in "${typesarr[@]}"; do
    make ACTION=${type} clean
  done

  # dry run to load files into RAM
  echo "--- Dry run for ${bm} (input '${in}') ---"
  make ACTION=native
  ./build/native/${bm} ${in}

  for threads in "${threadsarr[@]}"; do
    for type in "${typesarr[@]}"; do

      make ACTION=${type}
      echo "--- Running ${bm} ${threads} ${type} (input: '${in}') ---"

      # physical cores start with 0, so be compliant
      realthreads=$((threads-1))
      ${action} taskset -c 0-${realthreads} ./build/${type}/${bm} ${in}

    done  # type
  done  # threads

  cd ../
done # benchmarks

done # times
