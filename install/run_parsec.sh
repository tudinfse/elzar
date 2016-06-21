#!/usr/bin/env bash

# run
cd /root/bin/benchmarks/parsec-3.0
source env.sh

cd ${ELZAR}src/benches/parsec
rm -f /data/parsec.log
./run.sh &> /data/parsec.log

# collect
cd ${ELZAR}install
./collect_parsec.py
