#!/usr/bin/env bash
# Regenerates the synthetic test clip streamed by run_demo.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p testdata
ffmpeg -y -f lavfi -i "testsrc=size=640x360:rate=25:duration=6" \
       -pix_fmt yuv420p -c:v libx264 testdata/sample.mp4
