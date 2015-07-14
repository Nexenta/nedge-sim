#!/bin/bash
./test ngs $3 chunk_size $2 gateways $4 duration 10 seed $5  > $1.run$5.txt
tar zcf $1.run$5.tgz $1.run$5.txt log.csv bid.csv

