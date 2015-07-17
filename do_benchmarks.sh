#!/bin/bash
#############
name="gw64c4K"
#############
[ -f $name.txt ] && rm $name.txt
set -x
./test terse gateways 64 chunk_size 4 ngs 128 duration 20 seed 5791 | tee $name.txt
set +x
[ -f $name.tgz ] && rm $name.tgz
tar zcf $name.tgz $name.txt log.csv
#############
name="gw512c4K"
#############
[ -f $name.txt ] && rm $name.txt
set -x
./test terse gateways 512 chunk_size 4 ngs 128 duration 20 seed 5791 | tee $name.txt
set +x
[ -f $name.tgz ] && rm $name.tgz
tar zcf $name.tgz $name.txt log.csv
#############
name="gw64c128K"
#############
[ -f $name.txt ] && rm $name.txt
set -x
./test terse gateways 64 chunk_size 128 ngs 128 duration 20 seed 5791 | tee $name.txt
set +x
[ -f $name.tgz ] && rm $name.tgz
tar zcf $name.tgz $name.txt log.csv
#############
name="gw512c128K"
#############
[ -f $name.txt ] && rm $name.txt
set -x
./test terse gateways 512 chunk_size 128 ngs 128 duration 20 seed 5791 | tee $name.txt
set +x
[ -f $name.tgz ] && rm $name.tgz
tar zcf $name.tgz $name.txt log.csv
