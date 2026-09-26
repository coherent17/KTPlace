# KTPlace - put the built binary on PATH (bash / sh)
#
# Usage, from anywhere:
#     source ktplace.sh
#
# This file is meant to be *sourced*, not executed: running it would change
# PATH only inside a subshell that then exits, which is why it is
# deliberately not marked executable.
#
# After sourcing, the engine can be called by name:
#     ktplace ibm01 ./benchmark/ICCAD04/ibm01 ./output/ibm01.pl

# Resolve the repository root from this file's own location, so PATH is right
# no matter which directory it is sourced from.
if [ -n "${BASH_SOURCE[0]:-}" ]; then
    _ktplace_self="${BASH_SOURCE[0]}"
else
    _ktplace_self="$0"
fi
KTPLACE_HOME="$(cd "$(dirname "$_ktplace_self")" && pwd)"
export KTPLACE_HOME

_ktplace_bin="${KTPLACE_HOME}/build/bin"

if [ ! -x "${_ktplace_bin}/ktplace" ]; then
    echo "ktplace: ${_ktplace_bin}/ktplace not found; run 'make' first." >&2
else
    # Prepend only when the directory is not already on PATH, so re-sourcing
    # is a no-op instead of stacking duplicate entries.
    case ":${PATH}:" in
        *":${_ktplace_bin}":*) : ;;
        *) PATH="${_ktplace_bin}:${PATH}"; export PATH ;;
    esac
fi

unset _ktplace_self _ktplace_bin
