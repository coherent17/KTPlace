read_macro_constraints
foreach weight {0.02 0.3 1.0} {
    add_option simpl macro_net_weight $weight
    gplace_design
    macro_place
}
place_design
check_macros
write_macros
write_placement
