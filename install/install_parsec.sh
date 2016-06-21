#!/usr/bin/env bash

echo "===== Installing dependencies ====="
apt-get update
apt-get install -y mercurial wget pkg-config gettext libbsd-dev libx11-dev x11proto-xext-dev libxext-dev libxt-dev libxi-dev libxmu-dev

# get parsec
echo "===== Downloading Parsec ====="
mkdir -p /root/bin/benchmarks

cd /data/

if [ -d "parsec-3.0" ]; then
    rm -rf parsec-3.0
fi

wget -nc http://parsec.cs.princeton.edu/download/3.0/parsec-3.0.tar.gz
tar -xzf parsec-3.0.tar.gz
ln -s /data/parsec-3.0 /root/bin/benchmarks/parsec-3.0


echo "===== Patching Parsec ====="
cd /root/bin/benchmarks/parsec-3.0
cp ${ELZAR}install/parsec/parsec-hgignore .hgignore
hg init && hg add *
echo -e "[ui]\nusername = Your Name <your@mail>" > .hg/hgrc
hg com -m "1"
hg import ${ELZAR}install/parsec/parsec-complete_20151117.patch -m "2"

cd bin/
chmod +x parsecperf
chmod +x parsecperftx
chmod +x parsecperfavx

# prepare Parsec for Hardening
echo "===== Building Parsec ====="
cd /root/bin/benchmarks/parsec-3.0
source env.sh

declare -a benchmarks=("blackscholes" "bodytrack" "canneal" "dedup" "ferret" "fluidanimate" "raytrace" "streamcluster" "swaptions" "vips" "x264")
declare -a typesarr=("clang" "clang-pthreads" "clang-pthreads-nosse")

set -e
for benchmark in "${benchmarks[@]}"; do
    for type in "${typesarr[@]}"; do
        parsecmgmt -a build -p ${benchmark} -c ${type}
    done  # type
done  # benchmarks
set +e


# get inputs and setup tests
echo "===== Preparing Input files ====="
cd ${ELZAR}src/benches/parsec
./copyinputs.sh
./collect.sh
