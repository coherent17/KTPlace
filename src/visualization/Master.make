# Master.make for visualization subdirectory (SVG/GIF plotting of placements).

# ============================================================================
# Configuration
# ============================================================================

# Use variables passed from parent Makefile, or set defaults
CXX ?= g++
CXXFLAGS ?= -std=c++23 -Wall -Wextra -O2 -g -pthread
# Every source uses project-root-relative includes ("datamodel/kt_dm.h",
# "placer/simpl/kt_simpl.h", ...), so a single -I at the project root
# (src/) is all that is needed, regardless of which subdirectory make
# is running in.  `override` is required because the parent makefile
# passes INCLUDES on the command line.
override INCLUDES := -I..
OBJ_DIR ?= ../build/obj/visualization

# CImg is vendored in this directory and reached through the -I.. above; it is
# header-only, and the parent Makefile already links -lz for its PNG support.
# cimg_display is defined at the top of kt_plotter.cc (its only user) so the
# build never tries to open an X11 display.

# Source files in this directory
SRCS := kt_plotter.cc kt_gif.cc kt_animator.cc kt_plotOptions.cc

# Only include files that exist
EXISTING_SRCS := $(foreach src,$(SRCS),$(if $(wildcard $(src)),$(src),))

# Object files
OBJS := $(EXISTING_SRCS:%.cc=$(OBJ_DIR)/%.o)

# Dependency files
DEPS := $(OBJS:.o=.d)

# ============================================================================
# Targets
# ============================================================================

.PHONY: all clean objlist

# Default: build objects
all: $(OBJS)

# Compile source files
$(OBJ_DIR)/%.o: %.cc
	@mkdir -p $(OBJ_DIR)
	@echo "  Compiling $<..."
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

# Include dependency files
-include $(DEPS)

# Clean this directory
clean:
	@rm -f $(OBJS) $(DEPS)

# Return object list (used by parent Makefile)
objlist:
	@echo "$(OBJS)"