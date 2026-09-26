# KTPlace - put the built binary on PATH (csh / tcsh)
#
# Usage, from anywhere:
#     source ktplace.csh
#
# This file is meant to be *sourced*, not executed: running it would change
# PATH only inside a subshell that then exits, which is why it is
# deliberately not marked executable.
#
# After sourcing, the engine can be called by name:
#     ktplace ibm01 ./benchmark/ICCAD04/ibm01 ./output/ibm01.pl

# Locate the repository root. $_ is the file being sourced, so its ":r" root
# modifier gives the directory; if that is unavailable fall back to $cwd.
set ktplace_root = "$cwd"
if ( "$_" != "" ) then
    set ktplace_dir = "$_":r
    if ( -d "$ktplace_dir" ) set ktplace_root = "$ktplace_dir"
endif

set ktplace_bin = "$ktplace_root"/build/bin

if ( ! -x "$ktplace_bin/ktplace" ) then
    echo "ktplace: $ktplace_bin/ktplace not found; run 'make' first." >&2
else
    # Prepend only when it is not already first, so re-sourcing is a no-op.
    if ( "$PATH" !~ "$ktplace_bin"* ) then
        setenv PATH "$ktplace_bin":"$PATH"
    endif
    setenv KTPLACE_HOME "$ktplace_root"
endif

unset ktplace_root
unset ktplace_dir
unset ktplace_bin
