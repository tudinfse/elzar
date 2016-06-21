#!/bin/bash

BENCH=swaptions
NAME=$BENCH/$BENCH
ARGS=" -ns 4 -sm 4 -nt 2"
GOLDENRUN=$BENCH/goldenrun.log
GOLDENRUN_AVX=$BENCH/goldenrun_avx.log

source create_trace.sh

python fi-gdb.py -f -e -o 10000 -m nop  -p $NAME.native.exe -a "$ARGS" -d $NAME.native.log -r $GOLDENRUN -l logs/native &
wait
python fi-gdb.py -f -e -o 10000 -m nop  -p $NAME.avxswift.exe -a "$ARGS" -d $NAME.avxswift.log -r $GOLDENRUN_AVX -l logs/avxswift &
wait

