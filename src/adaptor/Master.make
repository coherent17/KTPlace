# Master.make for adaptor subdirectory

# ============================================================================
# Configuration
# ============================================================================

# Use variables passed from parent Makefile, or set defaults
CXX ?= g++
CXXFLAGS ?= -std=c++23 -Wall -Wextra -O2 -g -pthread
# Every source uses project-root-relative includes (see globalPlacer/Master.make).
override INCLUDES := -I.. $(THIRD_PARTY_INCLUDES)
OBJ_DIR ?= ../build/obj/adaptor

# Source files in this directory
SRCS := bookshelfToKTAdaptor.cc lefdefToKTAdaptor.cc kt_inputReader.cc kt_macroPlanIo.cc \
        kt_verilogNetlist.cc

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

