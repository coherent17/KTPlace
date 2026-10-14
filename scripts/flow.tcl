# flow.tcl -- the fixed flow of `ktplace <design>`, written as a script.
#
#   ktplace -t scripts/flow.tcl benchmark/ISPD_2005/adaptec1 -w output/adaptec1
#
# Produces the same placed.pl, plots and log records as running the design
# without -t (src/kt_flowMgr.cc), so it is the starting point for a custom flow:
# copy it and change the stages, or put add_option lines in front. `help` lists
# every command.

read_design $ktplace(input_dir)
report_design

# The placer holds back part of the animation's frame budget for the stages
# after it, and legalize releases it.
global_place -algo $ktplace(algorithm)
legalize
check_placement "after legalization"
detail_place
check_placement "after detail placement"

# After the last stage, so the pictures show the placement that is written.
write_final_image
write_animation
write_design $ktplace(output)
report_timing
