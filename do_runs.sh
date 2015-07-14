#!/bin/bash
set -v
./do_run_batch.sh g64c4K 4 128 64
./do_run_batch.sh g64c128K 128 128 64
./do_run_batch.sh g64c2M 2048 128 64
./do_run_batch.sh g256c4K 4 128 256
./do_run_batch.sh g256c128K 128 128 256
./do_run_batch.sh g256c2M 2048 128 256
