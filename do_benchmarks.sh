#!/bin/bash
#############
name="gw64c4K"
#############
[ -f $name.txt ] && rm $name.txt
set -x
./test terse gateways 64 chunk_size 4 ngs 128 duration 24 seed 5791 | tee $name.txt
set +x
[ -f $name.tgz ] && rm $name.tgz
tar zcf $name.tgz $name.txt log.csv
#############
name="gw256c4K"
#############
[ -f $name.txt ] && rm $name.txt
set -x
./test terse gateways 256 chunk_size 4 ngs 128 duration 24 seed 5791 | tee $name.txt
set +x
[ -f $name.tgz ] && rm $name.tgz
tar zcf $name.tgz $name.txt log.csv
#############
name="gw64c128K"
#############
[ -f $name.txt ] && rm $name.txt
set -x
./test terse gateways 64 chunk_size 128 ngs 128 duration 24 seed 5791 | tee $name.txt
set +x
[ -f $name.tgz ] && rm $name.tgz
tar zcf $name.tgz $name.txt log.csv
#############
name="gw256c128K"
#############
[ -f $name.txt ] && rm $name.txt
set -x
./test terse gateways 256 chunk_size 128 ngs 128 duration 24 seed 5791 | tee $name.txt
set +x
[ -f $name.tgz ] && rm $name.tgz
tar zcf $name.tgz $name.txt log.csv
