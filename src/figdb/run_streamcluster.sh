#!/bin/bash

BENCH=streamcluster
NAME=$BENCH/$BENCH
ARGS=" 2 5 1 10 10 5 none streamcluster_result.txt 2"
GOLDENRUN=$BENCH/goldenrun.txt

source create_trace.sh

python fi-gdb.py -f -b "streamcluster_result.txt" -o 10000 -m nop  -p $NAME.native.exe -a "$ARGS" -d $NAME.native.log -r $GOLDENRUN -l logs/native &
wait
python fi-gdb.py -f -b "streamcluster_result.txt" -o 10000 -m nop  -p $NAME.avxswift.exe -a "$ARGS" -d $NAME.avxswift.log -r $GOLDENRUN -l logs/avxswift &
wait

