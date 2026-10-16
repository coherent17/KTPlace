#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <design dir> <placed.pl>" >&2
    exit 2
fi

here=$(cd "$(dirname "$0")" && pwd)
design=$(cd "$1" && pwd)
solution=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
name=$(basename "$design")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

for ext in nodes pl nets scl; do
    if [[ -f "$design/$name.$ext.gz" ]]; then
        gzip -dc "$design/$name.$ext.gz" > "$work/$name.$ext"
    else
        cp "$design/$name.$ext" "$work/$name.$ext"
    fi
done

cd "$work"
perl "$here/hpwl.pl" "$name.nodes" "$name.pl" "$solution" "$name.nets" | grep "Total HPWL"
perl "$here/legal.pl" "$name.nodes" "$name.pl" "$solution" "$name.scl" > /dev/null
errors=$(grep -c "ERROR" legality.error || true)
echo "Legality: $errors error(s)"
[[ "$errors" -eq 0 ]]
