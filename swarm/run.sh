#!/bin/sh
# Start the swarm in the background:  swarm/run.sh <file holding the API key>
# Stop it:  touch build/swarm/STOP   (it stops after the current round; spend so far is in build/swarm/status.md)
# Environment (optional): SWARM_DIR, SWARM_CAP (dollars, default 20), SWARM_WORKERS (8), SWARM_MODEL, SWARM_API_URL.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
export SWARM_ROOT="$ROOT" SWARM_DIR="${SWARM_DIR:-$ROOT/build/swarm}" FUT="$ROOT/build/fut"
python3 "$ROOT/build.py" >/dev/null
mkdir -p "$SWARM_DIR"
rm -f "$SWARM_DIR/STOP"
SWARM_API_KEY="$(cat "$1")"
export SWARM_API_KEY
nohup "$FUT" "$ROOT/swarm/swarm.fut" >> "$SWARM_DIR/log.txt" 2>&1 &
echo $! > "$SWARM_DIR/pid"
echo "swarm running (pid $!): tail -f $SWARM_DIR/log.txt; status in $SWARM_DIR/status.md; stop with: touch $SWARM_DIR/STOP"
