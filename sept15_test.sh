./test gateways $1 ngs $2 chunk_size $3 gateway_mbs 0 mbs 1200 seed 123464 duration 50 outprefix G$1N$2C$3.chucast chucast | tee G$1N$2C$3.chucast.txt
./test gateways $1 ngs $2 chunk_size $3 gateway_mbs 0 mbs 1200 seed 123464 duration 50 outprefix G$1N$2C$3.rep rep | tee G$1N$2C$3.rep.txt


