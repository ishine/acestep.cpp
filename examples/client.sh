#!/bin/bash
# Test ace-server: LM enriches 2 variations, synth renders each to MP3
# Start the server first (./server.sh), then run this

set -eu

curl -sf http://127.0.0.1:8085/lm \
     -H "Content-Type: application/json" \
     -d @simple-batch.json \
     -o server-lm-out.json

N=$(jq length server-lm-out.json)
for i in $(seq 0 $((N - 1))); do
    jq ".[$i]" server-lm-out.json \
    | curl -sf http://127.0.0.1:8085/synth \
        -H "Content-Type: application/json" \
        -d @- \
        -o "server${i}.mp3"
done
