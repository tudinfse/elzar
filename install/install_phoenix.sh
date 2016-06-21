#!/usr/bin/env bash

apt-get update
apt-get install -y mercurial wget

mkdir -p /root/bin/benchmarks/
cd /root/bin/benchmarks/
hg clone https://bitbucket.org/dimakuv/phoenix-pthreads
cd phoenix-pthreads
make CONFIG=gcc
make CONFIG=gcc_nosse

export HOME='/root'
make CONFIG=clang
make CONFIG=clang_nosse

cd ${ELZAR}src/benches/phoenix_pthread
./copyinputs.sh
./collect.sh

