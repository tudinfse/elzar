#!/bin/bash

#==============================================================================#
# collect main *.bc files from Phoenix-pthreads builds
#==============================================================================#

set -x #echo on

PHOENIXPATH=${HOME}/bin/benchmarks/phoenix-pthreads
BENCHPATH=.

declare -a benchmarks=("histogram" "kmeans" "linear_regression" "matrix_multiply" "pca" "string_match" "word_count")
declare -a phoenixconfs=("clang" "clang_nosse" "clang_nosse" "clang_nosse" "clang_nosse" "clang_nosse")
declare -a benchconfs=("native" "native_nosse" "swift" "swiftr" "simdswift" "avxswift")

for bm in "${benchmarks[@]}"; do
  for confidx in "${!benchconfs[@]}"; do
    benchconf="${benchconfs[$confidx]}"
    phoenixconf="${phoenixconfs[$confidx]}"

    mkdir -p ${BENCHPATH}/${bm}/build/${benchconf}/

    cp ${PHOENIXPATH}/${bm}/build/${phoenixconf}/${bm}.bc  ${BENCHPATH}/${bm}/build/${benchconf}/
  done
done

