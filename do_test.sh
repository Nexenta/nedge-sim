#/usr/bin/bash
#do_test.sh <name> <gateways> <chunk_size> <ngs> <duration> <seed> 
name=$1
gws=$2
chunk_size=$3
ngs=$4
penalty=$5
gateway_mbs=$6
[ -f $name.txt ] && rm $name.txt
set -x
./test terse gateways $gws chunk_size $chunk_size ngs $ngs penalty $penalty gateway_mbs $gateway_mbs seed 5791 duration 50 | tee $name.txt
[ -f $name.tgz ] && rm $name.tgz
tar zcf $name.tgz $name.txt log.csv bid.csv inflight.csv
