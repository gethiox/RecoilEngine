#!/bin/bash
# This file is part of the Spring engine (GPL v2 or later), see LICENSE.html
#
# Diagnose a divergence right after loading: run the reference game and the
# loaded game for one more frame (save frame + 1) and compare
#  - DumpState output of that frame
#  - the complete serialized state after that frame (compare_states.py)
#  - the Lua heaps after that frame
#
# usage: diagnose_first_frame.sh <work-dir of run_integrity.py> <save-frame> <engine> <data-dir>
set -u
WORK=$1; SAVE=$2; ENGINE=$3; DATA=$4
NEXT=$((SAVE + 1))
HERE=$(cd "$(dirname "$0")" && pwd)

for r in ref load; do
	d=$WORK/diag-$r
	rm -rf "$d"; mkdir -p "$d"
	cat > "$d/springsettings.cfg" <<CFG
SimStateTestDisableLuaUI = 1
SimStateTestGameSpeed = 1000
SimStateTestQuitFrame = $((NEXT + 1))
SimStateTestCompareFrame = $NEXT
SimStateTestDumpFrames = $SAVE $NEXT 1
SimStateTestChecksumLog = $d/checksums.log
CFG
	if [ $r = ref ]; then arg=$WORK/script.txt; else arg=$WORK/save/Saves/snap.ssf; fi
	(cd "$d" && timeout 1200 "$ENGINE" --isolation --isolation-dir "$DATA" --write-dir "$d" --config "$d/springsettings.cfg" "$arg" > stdout.txt 2>&1)
done

R=$(ls $WORK/diag-ref/*GameState*); L=$(ls $WORK/diag-load/*GameState*)
echo "=== DumpState diff, frame $NEXT (first 60 lines)"
diff <(sed -n "/^frame: $NEXT/,\$p" "$R") <(sed -n "/^frame: $NEXT/,\$p" "$L") | head -60
echo "=== full state comparison after frame $NEXT"
python3 -I "$HERE/compare_states.py" "$WORK/diag-ref/compare-$NEXT" "$WORK/diag-load/compare-$NEXT" --max 30
echo "=== Lua heap diff (first 30 lines)"
diff "$WORK/diag-ref/compare-$NEXT.luarules.txt" "$WORK/diag-load/compare-$NEXT.luarules.txt" | head -30
