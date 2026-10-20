#!/usr/bin/env bash
# Score a macro placement the way ICCAD 2022 Problem D does: copy its macros into the
# case's Bookshelf .pl as fixed objects, let ntuplace3 place the standard cells, and
# report ntuplace3's final HPWL.
#
#   score.sh <bookshelf case dir> <macros: .dmp, or the .mlist for the baseline> <work dir> [ntuplace3]
#
# ntuplace3 defaults to benchmark/bookshelf/ICCAD22_D/ntuplace3, the copy that
# patch_ntuplace3.py makes runnable on kernels without vsyscall.
set -euo pipefail

if [[ $# -lt 3 || $# -gt 4 ]]; then
    sed -n '6p' "$0" | sed 's/^#   /usage: /' >&2
    exit 2
fi

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
design=$(cd "$1" && pwd)
macros=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
work=$3
ntuplace3=$(realpath "${4:-$root/benchmark/bookshelf/ICCAD22_D/ntuplace3}")
name=$(basename "$design")

mkdir -p "$work"
work=$(cd "$work" && pwd)
for ext in aux nodes nets wts pl scl shapes route; do
    if [[ -f "$design/$name.$ext.gz" ]]; then
        gzip -dc "$design/$name.$ext.gz" > "$work/$name.$ext"
    else
        cp "$design/$name.$ext" "$work/$name.$ext"
    fi
done

gzip -dcf "$macros" |
    awk '$1 == "-" { name = $2 } ($1 == "+" && ($2 == "PLACED" || $2 == "FIXED")) { print name, $4, $5, $7 }' \
        > "$work/macros.txt"

awk 'NR == FNR { x[$1] = $2; y[$1] = $3; o[$1] = $4; next }
     ($1 in x) { print $1, x[$1], y[$1], ":", o[$1], "/FIXED"; next }
     { print }' "$work/macros.txt" "$work/$name.pl" > "$work/$name.pl.scored"
mv "$work/$name.pl.scored" "$work/$name.pl"
echo "$(wc -l < "$work/macros.txt") macro(s) from $macros"

cd "$work"
start=$(date +%s)
"$ntuplace3" -aux "$name.aux" > ntuplace3.log 2>&1
echo "ntuplace3 finished in $(( $(date +%s) - start )) s"
grep -E '^ +HPWL=' ntuplace3.log | tail -1 | awk '{ print "HPWL", $2 }'
