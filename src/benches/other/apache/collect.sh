#!/bin/bash

#==============================================================================#
# collect httpd.bc from Apache httpd builds (native and native_nosse)
#==============================================================================#

set -x #echo on

HTTPDPATH=~/bin/httpd
BENCHPATH=.

declare -a apacheconfs=("native" "native_nosse" "native" "native_nosse" "native_nosse" "native_nosse")
declare -a benchconfs=("native" "native_nosse" "swift" "swift_nosse" "simdswift" "avxswift")

for confidx in "${!benchconfs[@]}"; do
  benchconf="${benchconfs[$confidx]}"
  apacheconf="${apacheconfs[$confidx]}"

  mkdir -p ${BENCHPATH}/build/${benchconf}/

  cp ${HTTPDPATH}/${apacheconf}/httpd/.libs/httpd.bc  ${BENCHPATH}/build/${benchconf}/
done
