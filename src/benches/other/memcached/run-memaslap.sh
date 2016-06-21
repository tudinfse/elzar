#!/bin/bash

#==============================================================================#
# Run memcached:
#     - start memcached server in background (under perf)
#     - start memaslap
#     - kill memcached server
#     - save memaslap stats and server logs in separate files
#==============================================================================#

#set -x #echo on

#============================== PARAMETERS ====================================#
bm="memcached"

#action="/usr/bin/time -p"
action="perf stat -e cycles,instructions -e branches,branch-misses -e r7C6"

declare -a threadsarr=(1 2 4 8 12 16)
declare -a clientsarr=(32 64 128 192 256 320 384 448 512)
declare -a typesarr=("native" "native_nosse" "avxswift")

declare -a inputsarr=("memaslap-default")

#========================== EXPERIMENT SCRIPT =================================#
echo "===== Results for memcached-local benchmark ====="

# remove old logs
rm -f memaslap.log

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
        echo "--- Running ${bm} type: ${type} threads: ${threads} clients: ${clients} input: ${in} ---" >> memaslap.log

        make ACTION=${type}
        ${action} ./build/${type}/memcached -t ${threads} & 

        sleep 1
        memaslap -s 127.0.0.1:11211 -t 30s -c ${clients} -T 8 >> memaslap.log 

        # kill server
        sleep 1
        killall memcached

      done # clients
    done  # threads
  done # types
done # inputs

done # times
