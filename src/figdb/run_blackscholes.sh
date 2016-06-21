#!/bin/bash

BENCH=blackscholes
NAME=$BENCH/$BENCH
ARGS=" 2 $BENCH/input/in_4.txt blackscholes_result.txt"
GOLDENRUN=$BENCH/goldenrun.log

source create_trace.sh

python fi-gdb.py -f -b "blackscholes_result.txt" -o 10000 -m nop  -p $NAME.native.exe -a "$ARGS" -d $NAME.native.log -r $GOLDENRUN -l logs/native &
wait
python fi-gdb.py -f -b "blackscholes_result.txt" -o 10000 -m nop  -p $NAME.avxswift.exe -a "$ARGS" -d $NAME.avxswift.log -r $GOLDENRUN -l logs/avxswift &
wait

