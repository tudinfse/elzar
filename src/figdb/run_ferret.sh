#!/bin/bash

BENCH=ferret
NAME=$BENCH/$BENCH
ARGS=" $BENCH/input/corel lsh $BENCH/input/queries 5 5 1 output.txt 2"
GOLDENRUN=$BENCH/goldenrun.log

source create_trace.sh

python fi-gdb.py -f -b "output.txt" -o 10000 -m nop  -p $NAME.native.exe -a "$ARGS" -d $NAME.native.log -r $GOLDENRUN -l logs/native &
wait
python fi-gdb.py -f -b "output.txt" -o 10000 -m nop  -p $NAME.avxswift.exe -a "$ARGS" -d $NAME.avxswift.log -r $GOLDENRUN -l logs/avxswift &
wait

