# One of each type an option can hold. The value is typed by how it is spelled,
# so a component reads it as the type it expects.
#
#   64        an int
#   1.5e-3    a double
#   0         a bool, because that is what a flag is written as
#   {a name with spaces}   a string, because braces suppress substitution

add_option simpl grid 64
add_option simpl alpha_base 1.5e-3
add_option simpl fences 0
add_option simpl tag {a name with spaces}
