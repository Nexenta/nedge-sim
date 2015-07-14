#!/bin/bash
./test ngs $3 chunk_size $2 gateways $4 duration 10 seed $5  > $1.run$5.txt
rm $1.run$5.tgz
tar zcf $1.run$5.tgz $1.run$5.txt log.csv bid.csv

