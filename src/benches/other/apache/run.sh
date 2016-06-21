#!/bin/bash

#==============================================================================#
# Run apache httpd:
#     - start httpd server in background
#         * master process forks 1 child process, which has 1 listener thread
#           and N worker threads (configured in httpd.conf)
#     - start ab (Apache Benchmark) client to generate requests for test page
#     - stop httpd server
#     - save ab client and server (perf stat) logs in separate files
#   - native & swift & trans & hard versions
#==============================================================================#

#set -x #echo on

#============================== PARAMETERS ====================================#
bm="httpd"
httpddir="${HOME}/bin/apache/bin"

#action="/usr/bin/time -p"
action="perf stat -e cycles,instructions -e branches,branch-misses -e r7C6"

declare -a threadsarr=(1 2 4 8 12 16)
declare -a clientsarr=(1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20)
declare -a typesarr=("native" "native_nosse" "avxswift")

declare -a inputsarr=(\
"llvm_language_ref.html" \
)

#========================== EXPERIMENT SCRIPT =================================#
echo "===== Results for httpd-local benchmark ====="

cp -f conf/httpd.conf ${httpddir}/../conf/
source ${httpddir}/envvars

# remove old logs
rm -f client.log

# remake the bench
for type in "${typesarr[@]}"; do
  make ACTION=${type} clean
done

for times in {1..${NUM_RUNS}}; do


for in in "${inputsarr[@]}"; do
  for type in "${typesarr[@]}"; do
    for threads in "${threadsarr[@]}"; do
      for clients in "${clientsarr[@]}"; do

        # rebuild if needed and copy to apache root dir
        sleep 5
        echo "--- Running ${bm} type: ${type} threads: ${threads} clients: ${clients} input: ${in} ---"
        echo "--- Running ${bm} type: ${type} threads: ${threads} clients: ${clients} input: ${in} ---" >> client.log

        make ANALYZE=1 ACTION=${type}
        cp -f ./build/${type}/httpd ${httpddir}/

        # start httpd server under perf (note: perf monitors also children)
        sleep 1
        ${action} ${httpddir}/httpd -DFOREGROUND -DThreads${threads} &

        # ab client
        sleep 1
        ab -k -c ${clients} -n 10000 http://127.0.0.1:8079/${in} >> client.log 2>&1

        # kill server
        killall httpd

      done # clients
    done  # threads
  done # types
done # inputs

done # times
