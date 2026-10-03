#!/bin/sh
# Runs the PC-1600 loader matrix: builds the harness, generates the programs,
# runs every manifest line and writes headless/loader-matrix/results1600.md
# (one row per run) plus a log per run in headless/loader-matrix/logs1600/.
# Usage: dev/loader-matrix/run_pc1600.sh
set -e
cd "$(dirname "$0")/../.."
dev/loader-matrix/build_pc1600.sh
python3 dev/loader-matrix/gen_pc1600.py
OUT=headless/loader-matrix
mkdir -p "$OUT/logs1600"
RES="$OUT/results1600.md"
echo "| Set | MODE | Slot 1 | Slot 2 | Via | Kind | Target | Tier | Bytes | Result | Notes |" > "$RES"
echo "|---|---|---|---|---|---|---|---|---|---|---|" >> "$RES"
pass=0; fail=0
while IFS="$(printf '\t')" read -r set mode s1 s2 via kind target tier size stream loader refuse; do
    log="$OUT/logs1600/set${set}_m${mode}_${s1}_${s2}_${via}_${kind}_${target}_${tier}.log"
    flag=""; [ "$refuse" = "True" ] && flag="--expect-refuse"
    set +e
    "$OUT/pc1600_loader_matrix" --mode "$mode" --slot1 "$s1" --slot2 "$s2" --transport "$via" \
        --kind "$kind" --stream "$stream" --loader-file "$loader" $flag > "$log" 2>&1
    set -e
    result=$(sed -n 's/^RESULT \([A-Z]*\).*/\1/p' "$log")
    [ -z "$result" ] && result=CRASH
    if [ "$result" = PASS ]; then pass=$((pass + 1)); else fail=$((fail + 1)); fi
    notes=""
    [ "$result" != PASS ] && notes=$(grep -E '^(ROM|LOADER):|diff: [1-9]' "$log" | grep -v '^work' | cut -c1-200 | tr '\n' ' ' | tr '|' '/')
    echo "| $set | $mode | $s1 | $s2 | $via | $kind | $target | $tier | $size | $result | $notes |" >> "$RES"
    printf '%s m%s %-8s %-8s %-5s %-5s %-12s %-4s %6s  %s\n' "$set" "$mode" "$s1" "$s2" "$via" "$kind" "$target" "$tier" "$size" "$result"
done < "$OUT/manifest1600.tsv"
echo "PASS $pass, FAIL $fail -> $RES"
