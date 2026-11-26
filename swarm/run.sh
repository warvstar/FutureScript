#!/bin/sh
# Start a swarm run in the background:  swarm/run.sh <scenario> [key file]   (key file default: build/swarm/.key)
#   Scenarios: swarm/scenarios/<name>.json, or as edited on the dashboard. It sets target, workers, model / models
#   (per role: worker, planner, approver, reviewer), effort (per role: low, medium, high), cap (dollars), minutes (0: none)
#   and cfTests (tests on the Worker).
# Stop it:  touch <run dir>/STOP   (it stops after the current round; spend so far is in <run dir>/status.md)
# Environment (optional, overrides the scenario): SWARM_DIR (default build/swarm-runs/<scenario>-<time>; an existing
#   directory resumes its run), SWARM_TARGET, SWARM_WORKERS, SWARM_MODEL, SWARM_MODEL_{WORKER,PLANNER,APPROVER,REVIEWER},
#   SWARM_CAP, SWARM_MINUTES, SWARM_GLOBAL_CAP (all runs together, default 40), SWARM_CF_URL (tests on that Worker),
#   SWARM_DASH_URL (publish the dashboard to that Worker; its controls then reach the swarm), SWARM_API_URL, SWARM_RESULTS.
# With SWARM_DASH_URL, a run that ends waits for the dashboard to start the next one; swarm/supervise.sh starts it.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
[ -n "$1" ] || { sed -n '2,11p' "$0"; exit 2; }
export SWARM_SCENARIO="$1"
KEYFILE="${2:-$ROOT/build/swarm/.key}"
[ -f "$ROOT/swarm/scenarios/$1.json" ] || [ -n "$SWARM_DASH_URL" ] || { echo "no scenario swarm/scenarios/$1.json"; exit 2; }
export SWARM_ROOT="$ROOT" FUT="$ROOT/build/fut" SWARM_KEY_FILE="$KEYFILE"
export SWARM_DIR="${SWARM_DIR:-$ROOT/build/swarm-runs/$1-$(date +%Y%m%d-%H%M%S)}"
export SWARM_CF_TOKEN_FILE="${SWARM_CF_TOKEN_FILE:-$ROOT/build/swarm/.cftoken}"
python3 "$ROOT/build.py" >/dev/null
mkdir -p "$SWARM_DIR"
rm -f "$SWARM_DIR/STOP"
nohup sh "$ROOT/swarm/supervise.sh" >> "$SWARM_DIR/log.txt" 2>&1 &
echo $! > "$SWARM_DIR/pid"
echo "swarm running (pid $!): $SWARM_DIR/dashboard.html${SWARM_DASH_URL:+, $SWARM_DASH_URL}; stop with: touch $SWARM_DIR/STOP"
