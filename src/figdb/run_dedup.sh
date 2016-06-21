#!/bin/bash

BENCH=dedup
NAME=$BENCH/$BENCH
ARGS=" -c -p -v -t 2 -i dedup/input/key_file.txt -o dedup_result.dat"
GOLDENRUN=$BENCH/goldenrun.dat

source create_trace.sh

python fi-gdb.py -f -b "dedup_result.dat" -o 10000 -m nop  -p $NAME.native.exe -a "$ARGS" -d $NAME.native.log -r $GOLDENRUN -l logs/native &
wait
python fi-gdb.py -f -b "dedup_result.dat" -o 10000 -m nop  -p $NAME.avxswift.exe -a "$ARGS" -d $NAME.avxswift.log -r $GOLDENRUN -l logs/avxswift &
wait

