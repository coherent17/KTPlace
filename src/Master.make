# KTPlace Master.make - Hierarchical build system for C++23
# Root makefile that coordinates all subdirectories (named Master.make for
# consistency with the per-component Master.make files)

# ============================================================================
# Configuration
# ============================================================================

# Compiler settings
CXX := g++
CXXFLAGS := -std=c++23 -Wall -Wextra -Wpedantic -Wshadow -O2 -g -pthread
# All quoted includes are project-root-relative (e.g. "datamodel/kt_dm.h"),
# so the project root (this directory) is the only include path needed.
INCLUDES := -I.

# External libraries (oneTBB for parallelism, Boost.Iostreams/zlib for gzip
# input, fmt for log message formatting)
LIBS := -ltbb -lboost_iostreams -lz -lfmt

# Directories
SRC_DIR := .
BUILD_DIR := ../build
OBJ_DIR := $(BUILD_DIR)/obj
BIN_DIR := $(BUILD_DIR)/bin
LIB_DIR := $(BUILD_DIR)/lib

# Subdirectories
SUBDIRS := datamodel adaptor placer visualization util

# Source files in current directory
LOCAL_SRCS := kt_flowMgr.cc kt_place.cc kt_option.cc

# ============================================================================
# Derived variables
# ============================================================================

# Object files for current directory
LOCAL_OBJS := $(LOCAL_SRCS:%.cc=$(OBJ_DIR)/%.o)

# Get object files from subdirectories via their Master.make files
SUBDIR_OBJS := $(foreach dir,$(SUBDIRS),$(shell $(MAKE) -s -C $(dir) -f Master.make objlist OBJ_DIR=$(OBJ_DIR)/$(dir)))

# All object files
ALL_OBJS := $(LOCAL_OBJS) $(SUBDIR_OBJS)

# Dependency files
DEPS := $(ALL_OBJS:.o=.d)

# Final targets
TARGET := $(BIN_DIR)/ktplace
STATIC_LIB := $(LIB_DIR)/libktplace.a

# ============================================================================
# Phony targets
# ============================================================================

.PHONY: all clean rebuild $(SUBDIRS) dirs help

# Default target
all: dirs $(SUBDIRS) $(TARGET) env

# Static library
lib: dirs $(SUBDIRS) $(STATIC_LIB)

# Create necessary directories
dirs:
	@mkdir -p $(OBJ_DIR) $(BIN_DIR) $(LIB_DIR)
	@mkdir -p $(foreach dir,$(SUBDIRS),$(OBJ_DIR)/$(dir))

# Build subdirectories
$(SUBDIRS):
	@echo "Building $@..."
	@$(MAKE) -C $@ -f Master.make OBJ_DIR=$(OBJ_DIR)/$@ INCLUDES="$(INCLUDES)" CXX="$(CXX)" CXXFLAGS="$(CXXFLAGS)"

# Main executable
$(TARGET): $(ALL_OBJS) | dirs
	@echo "Linking $@..."
	@$(CXX) $(CXXFLAGS) -o $@ $(ALL_OBJS) $(LIBS)
	@echo "Build complete: $@"

# Environment helper scripts.
#
# Copied to the repository root only after a successful link, so they never
# appear for a build that did not produce a binary. `cmp -s` keeps the copy
# from touching the timestamp when nothing changed, so sourcing stays cheap
# and make does not consider the target perpetually out of date.
TOP_DIR := $(abspath $(CURDIR)/..)
ENV_SCRIPTS := $(TOP_DIR)/ktplace.sh $(TOP_DIR)/ktplace.csh
ENV_SOURCES := $(TOP_DIR)/scripts/ktplace.sh $(TOP_DIR)/scripts/ktplace.csh

.PHONY: env
env: $(ENV_SCRIPTS)

$(ENV_SCRIPTS): $(ENV_SOURCES) | $(TARGET)
	@for src in $(ENV_SOURCES); do \
	    dst="$(TOP_DIR)/`basename $$src`"; \
	    if cmp -s "$$src" "$$dst"; then echo "  up to date: $$dst"; \
	    else cp "$$src" "$$dst" && echo "  generated: $$dst"; fi; \
	done

# Static library
$(STATIC_LIB): $(ALL_OBJS) | dirs
	@echo "Creating static library $@..."
	@ar rcs $@ $(ALL_OBJS)
	@echo "Library created: $@"

# Compile local source files
$(OBJ_DIR)/%.o: %.cc
	@echo "Compiling $<..."
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

# Include dependency files
-include $(DEPS)

# Clean
clean:
	@echo "Cleaning..."
	@rm -rf $(BUILD_DIR)
	@$(foreach dir,$(SUBDIRS),$(MAKE) -C $(dir) -f Master.make clean;)

# Rebuild
rebuild: clean all

# Help
help:
	@echo "KTPlace Build System"
	@echo "===================="
	@echo "Targets:"
	@echo "  all      - Build executable (default)"
	@echo "  lib      - Build static library"
	@echo "  clean    - Remove build artifacts"
	@echo "  rebuild  - Clean and rebuild"
	@echo "  help     - Show this help message"
	@echo ""
	@echo "Configuration:"
	@echo "  CXX      = $(CXX)"
	@echo "  CXXFLAGS = $(CXXFLAGS)"
	@echo "  LIBS     = $(LIBS)"
	@echo "  Build dir = $(BUILD_DIR)"

# Object list for dependency tracking (used by parent makefiles)
objlist:
	@echo "$(LOCAL_OBJS)"

