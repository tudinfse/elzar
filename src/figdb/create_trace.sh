#!/bin/bash
# this script must be called from run* scripts
FULL_PATH=~/code/fi-gdb/${NAME}

echo 0 | sudo tee /proc/sys/kernel/yama/ptrace_scope > /dev/null

~/bin/intel_sde/sde64 -rtm-mode nop -debugtrace -- ${FULL_PATH}.native.exe $ARGS
grep 'INS 0x0000000000' sde-debugtrace-out.txt > ${FULL_PATH}.native.log
~/bin/intel_sde/sde64 -rtm-mode nop -debugtrace -- ${FULL_PATH}.avxswift.exe $ARGS
grep 'INS 0x0000000000' sde-debugtrace-out.txt > ${FULL_PATH}.avxswift.log
rm -f sde-debugtrace-out.txt

