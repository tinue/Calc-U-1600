#!/bin/sh
# Runs the PC-1500(A) loader matrix: builds the harness, generates the
# programs, runs every manifest line and writes
# headless/loader-matrix/results.md (one row per run) plus a log per run in
# headless/loader-matrix/logs/. Usage: dev/loader-matrix/run_pc1500.sh
set -e
cd "$(dirname "$0")/../.."
dev/loader-matrix/build.sh
python3 dev/loader-matrix/gen_pc1500.py
OUT=headless/loader-matrix
mkdir -p "$OUT/logs"
RES="$OUT/results.md"
echo "| Model | Card | Kind | Tier | Bytes | Result | User-RAM diff | Notes |" > "$RES"
echo "|---|---|---|---|---|---|---|---|" >> "$RES"
pass=0; fail=0
while IFS="$(printf '\t')" read -r model card kind tier size stream loader refuse; do
    log="$OUT/logs/${model}_${card}_${kind}_${tier}.log"
    flag=""; [ "$refuse" = "True" ] && flag="--expect-refuse"
    set +e
    "$OUT/pc1500_loader_matrix" --model "$model" --card "$card" --kind "$kind" \
        --stream "$stream" --loader-file "$loader" $flag > "$log" 2>&1
    set -e
    result=$(sed -n 's/^RESULT \([A-Z]*\).*/\1/p' "$log")
    [ -z "$result" ] && result=CRASH
    if [ "$result" = PASS ]; then pass=$((pass + 1)); else fail=$((fail + 1)); fi
    diff=$(sed -n 's/^user RAM .* diff: \([0-9]*\) bytes.*/\1/p' "$log")
    notes=""
    [ "$result" != PASS ] && notes=$(grep -E '^(ROM|LOADER):' "$log" | tr '\n' ' ' | tr '|' '/')
    echo "| $model | $card | $kind | $tier | $size | $result | ${diff:--} | $notes |" >> "$RES"
    printf '%-9s %-7s %-6s %-5s %6s  %s\n' "$model" "$card" "$kind" "$tier" "$size" "$result"
done < "$OUT/manifest.tsv"
echo "PASS $pass, FAIL $fail -> $RES"
