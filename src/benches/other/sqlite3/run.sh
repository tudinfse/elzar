#!/bin/bash

#==============================================================================#
# Run sqlite3:
#     - start sqlite3 YCSB-based bench (also can run speedtest if uncomment)
#     - save log
#==============================================================================#

#set -x #echo on

#============================== PARAMETERS ====================================#
bm="sqlite3"

#action="/usr/bin/time -p"
action="perf stat -e cycles,instructions -e branches,branch-misses -e r7C6"

#declare -a threadsarr=(1)                    # only single-threaded in speedtest
declare -a threadsarr=(1 2 4 8 12 16)
declare -a typesarr=("native" "native_nosse" "avxswift")

#declare -a inputsarr=("speedtest-size1000")
inputsdir="${HOME}/bin/benchmarks/ycsb-traces"
declare -a inputsarr=(\
"a_kv1M_op1M" \
"b_kv1M_op1M" \
"c_kv1M_op1M" \
"d_kv1M_op1M" \
)

#========================== EXPERIMENT SCRIPT =================================#
echo "===== Results for sqlite3-local benchmark ====="

# re-make the bench
rm -rf build/

for times in {1..${NUM_RUNS}}; do


for in in "${inputsarr[@]}"; do
  for type in "${typesarr[@]}"; do
    for threads in "${threadsarr[@]}"; do

      make ACTION=${type}
      sleep 1
      echo "--- Running ${bm} type: ${type} threads: ${threads} input: ${in} ---"
#      ${action} ./build/${type}/sqlite3 -size 1000     # for speedtest
      ${action} ./build/${type}/sqlite3 -l ${inputsdir}/${in}.load -r ${inputsdir}/${in}.run -d 6.0 -t ${threads}

    done  # threads
  done # types
done # inputs

done # times
