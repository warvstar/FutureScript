#!/bin/sh
# Start the swarm in the background:  swarm/run.sh <file holding the API key>
# Stop it:  touch build/swarm/STOP   (it stops after the current round; spend so far is in build/swarm/status.md)
# Environment (optional): SWARM_TARGET (fsql; a spec in swarm/targets/), SWARM_DIR (build/swarm; a new directory starts a
#   new run from scratch), SWARM_CAP (dollars, default 20), SWARM_MINUTES (time limit, 0 = none), SWARM_WORKERS (8),
#   SWARM_MODEL (every role, default claude-haiku-5-5), SWARM_MODEL_{WORKER,PLANNER,APPROVER,REVIEWER} (one role),
#   SWARM_API_URL, SWARM_RESULTS (swarm/results).
# A benchmark run:  SWARM_DIR=build/swarm-fsql-2 SWARM_MINUTES=60 SWARM_CAP=5 swarm/run.sh <key file>
#   The hidden tests in swarm/targets/<target>/accept/ score main as it grows; swarm/results/<run>.json has the result.
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
