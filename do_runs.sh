#!/bin/bash
./do_run_batch.sh t16K4K 4 128 128 128
./do_run_batch.sh t16K8K 8 128 128 128
./do_run_batch.sh t16K16K 16 128 128 128
./do_run_batch.sh t16K128K 128 128 128 128
./do_run_batch.sh t16K1M 1024 128 128 128
./do_run_batch.sh t16K16M 16384 128 128 128
./do_run_batch.sh t32K4K 4 256 128 256
./do_run_batch.sh t32K8K 8 256 128 256
./do_run_batch.sh t32K16K 16 256 128 256
./do_run_batch.sh t32K128K 128 256 128 256
./do_run_batch.sh t32K1M 1024 256 128 256
./do_run_batch.sh t32K16M 16384 256 128 256
