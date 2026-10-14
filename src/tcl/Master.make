# Master.make for the tcl subdirectory: the libtcl command layer over ktSession
# (kt_tclShell), and the small configuration interpreter that fills the option
# store (kt_tcl, kt_tclMgr).

# ============================================================================
# Configuration
# ============================================================================

CXX ?= g++
CXXFLAGS ?= -std=c++23 -Wall -Wextra -O2 -g -pthread
# Every source uses project-root-relative includes (see placer/Master.make).
override INCLUDES := -I..
OBJ_DIR ?= ../build/obj/tcl
# Tcl 8.6 headers. Ubuntu ships no pkg-config file for them, so the path is
# named; override TCL_CFLAGS for another layout.
TCL_CFLAGS ?= -I/usr/include/tcl8.6

# Source files in this directory
SRCS := kt_tclShell.cc kt_tcl.cc kt_tclMgr.cc

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
	@$(CXX) $(CXXFLAGS) $(INCLUDES) $(TCL_CFLAGS) -MMD -MP -c $< -o $@

# Include dependency files
-include $(DEPS)

# Clean this directory
clean:
	@rm -f $(OBJS) $(DEPS)

# Return object list (used by parent Makefile)
objlist:
	@echo "$(OBJS)"
