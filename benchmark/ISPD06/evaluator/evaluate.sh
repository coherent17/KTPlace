#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
    echo "usage: $0 <design dir> <placed.pl> [target density]" >&2
    exit 2
fi

here=$(cd "$(dirname "$0")" && pwd)
design=$(cd "$1" && pwd)
solution=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
name=$(basename "$design")

declare -A targets=(
    [adaptec5]=0.5 [newblue1]=0.8 [newblue2]=0.9 [newblue3]=0.8
    [newblue4]=0.5 [newblue5]=0.5 [newblue6]=0.8 [newblue7]=0.8
)
target=${3:-${targets[$name]:-}}
if [[ -z "$target" ]]; then
    echo "no contest target density for $name; pass one as the third argument" >&2
    exit 2
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
for ext in nodes pl nets scl; do
    if [[ -f "$design/$name.$ext.gz" ]]; then
        gzip -dc "$design/$name.$ext.gz" > "$work/$name.$ext"
    else
        cp "$design/$name.$ext" "$work/$name.$ext"
    fi
done
cp "$here/transformed.list" "$work/"

cd "$work"
hpwl=$(perl "$here/hpwl.pl" "$name.nodes" "$name.pl" "$solution" "$name.nets" |
    sed -n 's/.*Total HPWL: *\([0-9.]*\).*/\1/p')
perl "$here/check_legality.pl" "$name.nodes" "$name.pl" "$solution" "$name.scl" 100 > /dev/null
errors=$(grep -c "ERROR" legality.error || true)

density=check_density_target.pl
[[ "$name" == newblue1 ]] && density=check_density_target_for_nw1.pl
overflow=$(perl "$here/$density" "$name.nodes" "$solution" "$name.scl" "$target" |
    sed -n 's/.*Scaled Overflow per bin: *//p')

echo "HPWL: $hpwl"
echo "Legality: $errors error(s)"
echo "Target density: $target"
echo "Scaled overflow per bin: $overflow"
awk -v h="$hpwl" -v o="$overflow" 'BEGIN { printf "Scaled HPWL: %.0f\n", h * (1 + o / 100) }'
[[ "$errors" -eq 0 ]]
