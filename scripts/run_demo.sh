#!/usr/bin/env bash
# Runs statsservice, rtp_sender, and rtp_receiver together over loopback
# so you can see the whole pipeline work end to end: encode -> RTP/UDP ->
# stats relay -> decode. Requires `make` to have built rtp_sender/
# rtp_receiver, `go build` for statsservice, and testdata/sample.mp4
# (see gen_testdata.sh).
set -euo pipefail
cd "$(dirname "$0")/.."

mkdir -p sdp
STATS_HTTP=${STATS_HTTP:-:8090}
RELAY_PORT=${RELAY_PORT:-6000}
RECEIVER_PORT=${RECEIVER_PORT:-6002}

./statsservice/statsservice -listen-rtp ":$RELAY_PORT" -relay-to "127.0.0.1:$RECEIVER_PORT" -http "$STATS_HTTP" &
STATS_PID=$!
trap 'kill $STATS_PID $SENDER_PID $RECEIVER_PID 2>/dev/null || true' EXIT
sleep 0.5

./rtp_sender testdata/sample.mp4 "rtp://127.0.0.1:$RELAY_PORT" sdp/sender.sdp &
SENDER_PID=$!

# Poll for the SDP file rather than a fixed sleep, so the receiver binds
# its socket as soon as possible after the sender starts (minimizes the
# window in which early packets arrive before anything is listening).
for _ in $(seq 1 100); do
  [ -s sdp/sender.sdp ] && break
  sleep 0.02
done
sed "s/^m=video $RELAY_PORT/m=video $RECEIVER_PORT/" sdp/sender.sdp > sdp/receiver.sdp

./rtp_receiver sdp/receiver.sdp testdata/received.yuv &
RECEIVER_PID=$!

wait $SENDER_PID
echo "--- live stats (http://localhost${STATS_HTTP}/stats) ---"
curl -s "http://localhost${STATS_HTTP}/stats"; echo
echo "--- waiting on receiver (Ctrl+C to stop early) ---"
wait $RECEIVER_PID
