#!/bin/bash
set -x
./test terse ngs $3 chunk_size $2 gateways $4 duration 100 seed $5 | tee $1.run$5.txt
set +x
[ -f "$1.run$5.tgz" ] && rm $1.run$5.tgz
tar zcf $1.run$5.tgz $1.run$5.txt log.csv bid.csv
