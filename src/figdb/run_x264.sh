#!/bin/bash

BENCH=x264
NAME=$BENCH/$BENCH
ARGS=" --quiet --qp 20 --partitions b8x8,i4x4 --ref 5 --direct auto --b-pyramid --weightb --mixed-refs --no-fast-pskip --me umh --subme 7 --analyse b8x8,i4x4 --threads 2 -o eledream.264 $BENCH/input/eledream_32x18_1.y4m"
GOLDENRUN=$BENCH/goldenrun.264

source create_trace.sh

python fi-gdb.py -f -b "eledream.264" -o 10000 -m nop  -p $NAME.native.exe -a "$ARGS" -d $NAME.native.log -r $GOLDENRUN -l logs/native &
wait
python fi-gdb.py -f -b "eledream.264" -o 10000 -m nop  -p $NAME.avxswift.exe -a "$ARGS" -d $NAME.avxswift.log -r $GOLDENRUN -l logs/avxswift &
wait

