# Master.make for legalizer subdirectory (post-placement legalization).
# Each legalizer in this directory is a self-contained module compiled into the
# executable; add new .cc files to SRCS.

# ============================================================================
# Configuration
# ============================================================================

CXX ?= g++
CXXFLAGS ?= -std=c++23 -Wall -Wextra -O2 -g -pthread
override INCLUDES := -I..
OBJ_DIR ?= ../build/obj/legalizer

# Source files in this directory
SRCS := abacus/abacus_legalizer.cc abacus/abacus_design.cc abacus/abacus_dm.cc \
        abacus/abacus_flowMgr.cc multirow/multirow_legalizer.cc multirow/multirow_design.cc multirow/multirow_dm.cc \
        multirow/multirow_flowMgr.cc

EXISTING_SRCS := $(foreach src,$(SRCS),$(if $(wildcard $(src)),$(src),))

ifeq ($(EXISTING_SRCS),$(SRCS))
OBJS := $(EXISTING_SRCS:%.cc=$(OBJ_DIR)/%.o)
else
$(error In legalizer/Master.make, these source files are listed in SRCS but do not exist: $(filter-out $(EXISTING_SRCS),$(SRCS)))
endif

# Dependency files
DEPS := $(OBJS:.o=.d)

# ============================================================================
# Targets
# ============================================================================

.PHONY: all clean objlist

all: $(OBJS) | $(sort $(dir $(OBJS)))

$(sort $(dir $(OBJS))):
	@mkdir -p $@

# Compile source files
$(OBJ_DIR)/%.o: %.cc
	@mkdir -p $(dir $@)
	@echo "  Compiling $<..."
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

-include $(DEPS)

clean:
	@rm -f $(OBJS) $(DEPS)

objlist:
	@echo "$(OBJS)"
