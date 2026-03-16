#!/bin/bash
# Generate 2 song variations via LM (batch_size=2 in JSON)
#
# LM phase (batch_size=2 in simple-batch.json):
# simple-batch.json -> simple-batch0.json, simple-batch1.json
# Each output has batch_size=1 (consumed by LM).
#
# DiT phase (batch_size=1 in each output JSON):
# simple-batch0.json -> simple-batch00.mp3
# simple-batch1.json -> simple-batch10.mp3

set -eu

# Phase 1: LM generates 2 variations (different lyrics/codes/metas)
../build/ace-lm \
    --request simple-batch.json \
    --model ../models/acestep-5Hz-lm-4B-Q8_0.gguf

# Phase 2: DiT+VAE renders each variation
../build/ace-synth \
    --request simple-batch0.json simple-batch1.json \
    --text-encoder ../models/Qwen3-Embedding-0.6B-Q8_0.gguf \
    --dit ../models/acestep-v15-turbo-Q8_0.gguf \
    --vae ../models/vae-BF16.gguf
