#!/usr/bin/env bash

# run
cd ${ELZAR}src/benches/phoenix_pthread
rm -f /data/phoenix.log
./run.sh &> /data/phoenix.log

# collect
cd ${ELZAR}install
./collect_phoenix.py
