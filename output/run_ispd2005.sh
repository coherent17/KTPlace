#!/bin/zsh
# Runs the ISPD 2005 suite; everything lands under this repo's output/.
#
# adaptec1 and ibm01 are vendored in benchmark/, so they always run. The other
# seven ISPD 2005 designs are not in the tree and have no working download URL,
# so they run only where someone has already unpacked them, and are reported as
# SKIP rather than failing.
# The per-design summary goes to output/_logs/suite.log and a one-line verdict for
# each design goes to stdout.
set -u

# The repo root, from this script's own location, so the suite runs the same
# wherever the checkout is rather than only where it was written.
ROOT="${0:A:h:h}"
cd "$ROOT" || exit 1

LOGDIR="$ROOT/output/_logs"
mkdir -p "$LOGDIR"
SUITE_LOG="$LOGDIR/suite.log"

# Suite-qualified, because the vendored designs are no longer all from one suite.
# ibm01 is here for the same reason it is in the tree at all: it is the only
# design these runs put through the multi-row legalizer.
DESIGNS=(
  ISPD_2005/adaptec1 ISPD_2005/adaptec2 ISPD_2005/adaptec3 ISPD_2005/adaptec4
  ISPD_2005/bigblue1 ISPD_2005/bigblue2 ISPD_2005/bigblue3 ISPD_2005/bigblue4
  ICCAD04/ibm01
)

# Animation on, and the frame budget raised well above the default: the suite is
# how the animation is looked at, and the default 300 frames on a 12-iteration
# run leaves the legalizer and the detailed placer unrepresented. The stills are
# removed once each GIF is written, so this costs no disk in the end.
run_one() {
  local name="$1" d="$2"
  rm -rf "output/$name"
  mkdir -p "output/$name"
  echo "=== START $name ($(date +%T))" | tee -a "$SUITE_LOG"

  # One positional input directory and a work directory; the binary derives
  # placed.pl, plots/ and ktplace.log from them. The old three-positional form
  # (<name> <dir> <out.pl>) died with the CLI refactor, and every design here
  # failed instantly with rc=1, which the summary below reported as a plain
  # "verdict=?" rather than as a broken invocation.
  KTPLACE_ANIM=1 KTPLACE_ANIM_MAX_FRAMES=1200 KTPLACE_ANIM_BLEND=2 \
  KTPLACE_SIMPL_TRACE_EVERY=1 KTPLACE_SIMPL_CG_EVERY=8 \
    ./build/bin/ktplace "$d" -w "output/$name" -a simpl > "$LOGDIR/$name.log" 2>&1
  local rc=$?

  # The stills are deleted once the GIF is written, so the frame count is read
  # from the run's own report rather than by counting files that are gone.
  local n
  n=$(grep -oE "animation: [0-9]+ frame" "$LOGDIR/$name.log" 2>/dev/null | grep -oE "[0-9]+")
  local g
  g=$(ls -la "output/$name/plots/anim/placement.gif" 2>/dev/null | awk '{print $5}')
  local v u p
  v=$(grep -oE "verdict *\| *[A-Z]+" "output/$name/ktplace.log" 2>/dev/null | grep -oE "[A-Z]+$")
  # Matched on the row's full name, not on the bare word "utilisation": that
  # word is now the start of two rows, "utilisation (movable / rows)" and
  # "utilisation (incl. fixed cells)", and grepping for the prefix silently
  # matched neither once the labels were disambiguated -- the suite printed
  # util=? and nobody could tell a naming change from a real regression.
  u=$(grep -oE "utilisation \(movable / rows\) *\| *[0-9.]+%" \
        "output/$name/ktplace.log" 2>/dev/null | grep -oE "[0-9.]+%" | head -1)
  # The high-resolution final still, when one was written.
  p=$(ls -la "output/$name/plots/final/final.png" 2>/dev/null | awk '{print $5}')

  echo "=== END $name rc=$rc frames=${n:-?} gif=${g:-none}B verdict=${v:-?} util=${u:-?} final=${p:-none}B ($(date +%T))" \
    | tee -a "$SUITE_LOG"
}

for entry in "${DESIGNS[@]}"; do
  name="${entry:t}"
  d="benchmark/$entry"
  if [[ ! -d "$d" ]]; then
    echo "=== SKIP $name (absent)" | tee -a "$SUITE_LOG"
    continue
  fi
  run_one "$name" "$d"
done

echo "=== SUITE DONE ($(date +%T))" | tee -a "$SUITE_LOG"
