# The shortest useful configuration: one option per line, typed by its spelling.
#
#   add_option <component> <key> <value>
#
# Run it with:  ktplace <design> -w out -c src/tcl/test/add_option_basic.tcl

add_option simpl grid 64
add_option simpl cg_tol 1e-6

add_option abacus max_row_dist 3
