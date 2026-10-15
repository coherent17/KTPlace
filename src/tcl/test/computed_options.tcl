# Options are ordinary Tcl: computed from variables, picked by a condition, or
# read out of a list. Only the final add_option for a key matters.

set cells 42120
set small_design [expr {$cells < 100000}]

# A default first, then narrowed for a design that needs it.
add_option simpl grid 32
if {$small_design} {
    add_option simpl grid 64
}

# A list walked to set options for more than one component at a time.
foreach {component key value} {
    abacus max_row_dist 3
    fastdp window 2
} {
    add_option $component $key $value
}

# A key computed like any other value.
set scale 4
add_option simpl cluster_scale $scale
