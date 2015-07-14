#!/bin/bash
./test terse ngs $3 chunk_size $2 gateways $4 duration 10 seed $5  > $1.run$5.txt
[ -f "$1.run$5.tgz" ] && rm $1.run$5.tgz
tar zcf $1.run$5.tgz $1.run$5.txt log.csv bid.csv
