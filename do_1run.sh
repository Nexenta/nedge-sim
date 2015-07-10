#!/bin/bash
./test ngs $3 targets_per 9 chunk_size $2 gateways  $4 mbs_sec 500 cluster_trip_time 350 seed 300$5 duration 2000 utilization $5 > $1.run$6.txt
tar zcf $1.run$6.tgz $1.run$6.txt log.csv bid.csv

