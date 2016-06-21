#!/bin/bash

#==============================================================================#
# collect main *.bc files from Parsec builds
#==============================================================================#

set -x #echo on

PARSECPATH=${HOME}/bin/benchmarks/parsec-3.0
BENCHPATH=.

declare -a benchmarks=("blackscholes" "bodytrack" "ferret" "fluidanimate" "swaptions" "vips" "x264" "canneal" "streamcluster" "dedup" "raytrace")
declare -a benchpaths=("apps" "apps" "apps" "apps" "apps" "apps" "apps" "kernels" "kernels" "kernels" "apps")
declare -a benchobjs=("blackscholes" "TrackingBenchmark/bodytrack" "parsec/bin/ferret-pthreads" "fluidanimate" "swaptions" "tools/iofuncs/vips" "x264" "canneal" "streamcluster" "dedup" "bin/rtview")

declare -a parsecconfs=("amd64-linux.clang-pthreads" "amd64-linux.clang-pthreads-nosse" "amd64-linux.clang-pthreads-nosse" "amd64-linux.clang-pthreads-nosse" "amd64-linux.clang-pthreads-nosse" "amd64-linux.clang-pthreads-nosse")
declare -a benchconfs=("native" "native_nosse" "swift" "swiftr" "simdswift" "avxswift")

for bmidx in "${!benchmarks[@]}"; do
  bm="${benchmarks[$bmidx]}"
  bmpath="${benchpaths[$bmidx]}"
  bmobj="${benchobjs[$bmidx]}"

  for confidx in "${!benchconfs[@]}"; do
    benchconf="${benchconfs[$confidx]}"
    parsecconf="${parsecconfs[$confidx]}"

    mkdir -p ${BENCHPATH}/${bm}/build/${benchconf}/

    cp ${PARSECPATH}/pkgs/${bmpath}/${bm}/obj/${parsecconf}/${bmobj}.bc  ${BENCHPATH}/${bm}/build/${benchconf}/
  done
done

