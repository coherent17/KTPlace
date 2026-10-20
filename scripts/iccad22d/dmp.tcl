read_macro_constraints
foreach round {1 2} {
    gplace_design
    macro_place
}
place_design
check_macros
write_macros
write_placement
