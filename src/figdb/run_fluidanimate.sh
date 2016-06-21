#!/bin/bash

BENCH=fluidanimate
NAME=$BENCH/$BENCH
ARGS=" 1 1 $BENCH/input/in_5K.fluid out.fluid"
GOLDENRUN=$BENCH/goldenrun.fluid

source create_trace.sh

python fi-gdb.py -f -b "out.fluid" -o 10000 -m nop  -p $NAME.native.exe -a "$ARGS" -d $NAME.native.log -r $GOLDENRUN -l logs/native &
wait
python fi-gdb.py -f -b "out.fluid" -o 10000 -m nop  -p $NAME.avxswift.exe -a "$ARGS" -d $NAME.avxswift.log -r $GOLDENRUN -l logs/avxswift &
wait

