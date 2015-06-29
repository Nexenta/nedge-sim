#!/bin/bash
./test ngs $3 targets_per 9 chunk_size $2 utilization 0 chunks_per_object $4 objects $5 mbs_sec 500 cluster_trip_time 350 seed 3001 > $1.run1.txt
tar zcf $1.run1.tgz $1.run1.txt log.csv bid.csv
./text ngs $3 targets_per 9 chunk_size $2 utilization 0 chunks_per_object $4 objects $5 mbs_sec 500 cluster_trip_time 350 seed 3002 > $1.run2.txt
tar zcf $1.run2.tgz $1.run2.txt log.csv bid.csv
./test ngs $3 targets_per 9 chunk_size $2  utilization 0 chunks_per_object $4 objects $5 mbs_sec 500 cluster_trip_time 350 seed 3003 > $1.run3.txt
tar zcf $1.run3.tgz $1.run3.txt log.csv bid.csv
./test ngs $3 targets_per 9 chunk_size $2  utilization 0 chunks_per_object $4 objects $5 mbs_sec 500 cluster_trip_time 350 seed 3004 > $1.run4.txt
tar zcf $1.run4.tgz $1.run4.txt log.csv bid.csv
./test ngs $3 targets_per 9 chunk_size $2  utilization 0 chunks_per_object $4 objects $5 mbs_sec 500 cluster_trip_time 350 seed 3005  > $1.run5.txt
tar zcf $1.run5.tgz $1.run5.txt log.csv bid.csv

