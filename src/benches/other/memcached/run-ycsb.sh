#!/bin/bash

#==============================================================================#
# Run memcached:
#     - start memcached server in background (under perf)
#     - start memcached client with load phase, wait till finish
#     - start memcached client with run phase,  wait till finish
#     - kill memcached server
#     - save client load&run phases and server logs in separate files
#==============================================================================#

#set -x #echo on

#============================== PARAMETERS ====================================#
bm="memcached"

#action="/usr/bin/time -p"
action="perf stat -e cycles,instructions -e branches,branch-misses -e r7C6"

declare -a threadsarr=(1 2 4 8 12 16)
declare -a clientsarr=(1 4 8 16 24 32 40 48)
declare -a typesarr=("native" "native_nosse" "avxswift")

inputsdir="${HOME}/bin/benchmarks/ycsb-traces"
declare -a inputsarr=(\
"a_kv1M_op1M" \
"b_kv1M_op1M" \
"c_kv1M_op1M" \
"d_kv1M_op1M" \
)

#========================== EXPERIMENT SCRIPT =================================#
echo "===== Results for memcached-local benchmark ====="

# remove old logs
rm -f ycsb-load.log
rm -f ycsb-run.log

# re-make the benches
rm -rf build/

for times in {1..${NUM_RUNS}}; do


for in in "${inputsarr[@]}"; do
  for type in "${typesarr[@]}"; do
    for threads in "${threadsarr[@]}"; do
      for clients in "${clientsarr[@]}"; do

        # rebuild if needed and start server
        sleep 1
        echo "--- Running ${bm} type: ${type} threads: ${threads} clients: ${clients} input: ${in} ---"
        echo "--- Running ${bm} type: ${type} threads: ${threads} clients: ${clients} input: ${in} ---" >> ycsb-load.log
        echo "--- Running ${bm} type: ${type} threads: ${threads} clients: ${clients} input: ${in} ---" >> ycsb-run.log

        make ACTION=${type}
        ${action} ./build/${type}/memcached -t ${threads} -v & 

        # client load phase, just need one thread to load all data
        sleep 1
        ./client/bench_client.exe -l ${inputsdir}/${in}.load -s 127.0.0.1 -d 6.0 -t 1 >> ycsb-load.log

        # client run phase
        sleep 1
        ./client/bench_client.exe -l ${inputsdir}/${in}.run  -s 127.0.0.1 -d 6.0 -t ${clients} >> ycsb-run.log

        # kill server
        sleep 1
        killall memcached

      done # clients
    done  # threads
  done # types
done # inputs

done # times
