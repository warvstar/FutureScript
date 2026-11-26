#!/bin/sh
# Runs swarm runs one after another (swarm/run.sh starts it): when a run ends and the dashboard picks the next
# scenario, swarm.fut writes its name to build/swarm-next and exits, and this starts that run in a new directory.
# The key is read from $SWARM_KEY_FILE for each run, so it never sits in this script's environment.
while true; do
  SWARM_API_KEY="$(cat "$SWARM_KEY_FILE")" "$FUT" "$SWARM_ROOT/swarm/swarm.fut" >> "$SWARM_DIR/log.txt" 2>&1
  NEXT="$SWARM_ROOT/build/swarm-next"
  [ -f "$NEXT" ] || exit 0
  SWARM_SCENARIO=$(cat "$NEXT")
  rm -f "$NEXT"
  # the next run is the scenario's alone: drop the first run's overrides
  unset SWARM_TARGET SWARM_WORKERS SWARM_MODEL SWARM_MODEL_WORKER SWARM_MODEL_PLANNER SWARM_MODEL_APPROVER SWARM_MODEL_REVIEWER SWARM_CAP SWARM_MINUTES
  SWARM_DIR="$SWARM_ROOT/build/swarm-runs/$SWARM_SCENARIO-$(date +%Y%m%d-%H%M%S)"
  export SWARM_SCENARIO SWARM_DIR
  mkdir -p "$SWARM_DIR"
  echo $$ > "$SWARM_DIR/pid"
done
