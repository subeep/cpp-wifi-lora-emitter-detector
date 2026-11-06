#!/usr/bin/env bash
# One over-the-air case: receiver (lora_live_check) + Heltec driver + matcher.
#   run_case.sh NAME RUN_TAG TX_SECONDS "RX ARGS" STEP...
# Writes data/lora_validation/<date>/NAME_{rx.json,rx.log,truth.ndjson,report.json}.
set -u
cd "$(dirname "$0")/../.."
NAME=$1; TAG=$2; TX=$3; RXARGS=$4; shift 4
D=${LORA_VALIDATION_DIR:-data/lora_validation/2026-11-06}
PY=${HELTEC_PY:-$HOME/.gemini/antigravity-ide/scratch/esp_env/bin/python}
mkdir -p "$D"
STEPS=(--step "0:RUN $TAG")
for s in "$@"; do STEPS+=(--step "$s"); done
build/lora_live_check --seconds $((TX + 25)) --json "$D/${NAME}_rx.json" $RXARGS > "$D/${NAME}_rx.log" 2>&1 &
RX=$!
trap 'kill $RX 2>/dev/null' EXIT
sleep 10
if ! kill -0 $RX 2>/dev/null; then
  echo "=== $NAME: receiver exited before transmitting; nothing sent" >&2
  tail -3 "$D/${NAME}_rx.log" >&2
  exit 1
fi
"$PY" tools/lora_validation/heltec_link.py --out "$D/${NAME}_truth.ndjson" --duration "$TX" --quiet "${STEPS[@]}"
wait $RX
trap - EXIT
echo "=== $NAME"
python3 tools/lora_validation/lora_match.py "$D/${NAME}_truth.ndjson" "$D/${NAME}_rx.json" --run "$TAG" --json "$D/${NAME}_report.json"
