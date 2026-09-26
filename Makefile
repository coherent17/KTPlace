# KTPlace Top-level Makefile
# This is the entry point for building the entire project

.PHONY: all clean rebuild test check help

# Default target - build from src directory
all:
	@$(MAKE) -C src -f Master.make

# Clean
clean:
	@$(MAKE) -C src -f Master.make clean

# Rebuild
rebuild:
	@$(MAKE) -C src -f Master.make rebuild

# Build and run the unit tests
test:
	@$(MAKE) -C src -f Master.make test

check: test

# Help
help:
	@echo "KTPlace Top-level Build System"
	@echo "=============================="
	@echo "Targets:"
	@echo "  all      - Build the project (default)"
	@echo "  clean    - Remove build artifacts"
	@echo "  rebuild  - Clean and rebuild"
	@echo "  test     - Build and run the unit tests (alias: check)"
	@echo "  help     - Show this help message"
	@echo ""
	@echo "Run 'make help' in src/ for more detailed options."

