#!/bin/bash
#
# penalty is Replicast chunk processing overhead above TCP connection establishment
# penalty here is set to 20000 or 2ms, which is 2 times CLUSTER_TRIP_TIME
# note: (2 * CLUSTER_TRIP_TIME) == RTT
#
#############
name="gw64c4K"
#############
[ -f $name.txt ] && rm $name.txt
set -x
./test terse gateways 64 chunk_size 4 ngs 128 duration 24 penalty 20000 seed 5791 | tee $name.txt
set +x
[ -f $name.tgz ] && rm $name.tgz
tar zcf $name.tgz $name.txt log.csv
#############
name="gw256c4K"
#############
[ -f $name.txt ] && rm $name.txt
set -x
./test terse gateways 256 chunk_size 4 ngs 128 duration 24 penalty 20000 seed 5791 | tee $name.txt
set +x
[ -f $name.tgz ] && rm $name.tgz
tar zcf $name.tgz $name.txt log.csv
#############
name="gw64c128K"
#############
[ -f $name.txt ] && rm $name.txt
set -x
./test terse gateways 64 chunk_size 128 ngs 128 duration 24 penalty 20000 seed 5791 | tee $name.txt
set +x
[ -f $name.tgz ] && rm $name.tgz
tar zcf $name.tgz $name.txt log.csv
#############
name="gw256c128K"
#############
[ -f $name.txt ] && rm $name.txt
set -x
./test terse gateways 256 chunk_size 128 ngs 128 duration 24 penalty 20000 seed 5791 | tee $name.txt
set +x
[ -f $name.tgz ] && rm $name.tgz
tar zcf $name.tgz $name.txt log.csv
#############
name="gw512c128K"
#############
[ -f $name.txt ] && rm $name.txt
set -x
./test terse gateways 512 chunk_size 128 ngs 128 duration 24 seed 5791 | tee $name.txt
set +x
[ -f $name.tgz ] && rm $name.tgz
tar zcf $name.tgz $name.txt log.csv
