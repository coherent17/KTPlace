#!/usr/bin/env bash
# Run one ktplace flow on ICCAD 2022 Problem D cases and score each .dmp with ntuplace3.
#
#   run_all.sh <flow.tcl> <label> [case ...]      (default: case01 ... case15)
#
# ktplace runs one case at a time (it uses every core); each finished .dmp is scored in
# the background, at most `jobs` ntuplace3 runs at once (one core and ~1.3 GB each).
# Results: output/<case>_<label>/ (ktplace) and output/<case>_<label>_ntuplace3/ (score),
# and a table in output/iccad22d_<label>.tsv.
set -euo pipefail

if [[ $# -lt 2 ]]; then
    sed -n '4p' "$0" | sed 's/^#   /usage: /' >&2
    exit 2
fi

root=$(cd "$(dirname "$0")/../.." && pwd)
flow=$(realpath "$1")
label=$2
shift 2
cases=("$@")
if [[ ${#cases[@]} -eq 0 ]]; then
    cases=(case01 case02 case03 case04 case05 case06 case07 case08 case09 case10 case11 case12 case13 case14 case15)
fi
jobs=8

cd "$root"
table=output/iccad22d_$label.tsv
printf 'case\tktplace\tntuplace3\n' > "$table"

for c in "${cases[@]}"; do
    design=benchmark/bookshelf/ICCAD22_D/$c
    work=output/${c}_$label
    build/bin/ktplace "$design" -w "$work" -tcl "$flow" > "$work.err" 2>&1 || {
        echo "$c: ktplace failed, see $work.err" >&2
        continue
    }
    echo "$c: ktplace done"
    while [[ $(jobs -rp | wc -l) -ge $jobs ]]; do
        sleep 10
    done
    scripts/iccad22d/score.sh "$design" "$work/$c.dmp" "${work}_ntuplace3" > "${work}_ntuplace3.txt" 2>&1 &
done
wait

for c in "${cases[@]}"; do
    work=output/${c}_$label
    ours=$(grep -E '^\| (detailed placement|legalization)' "$work/ktplace.log" 2>/dev/null | tail -1 | awk -F'|' '{ gsub(/ /, "", $3); print $3 }')
    theirs=$(awk '$1 == "HPWL" { print $2 }' "${work}_ntuplace3.txt" 2>/dev/null)
    printf '%s\t%s\t%s\n' "$c" "${ours:-?}" "${theirs:-?}" >> "$table"
done
column -t "$table"
