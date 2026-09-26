# KTPlace Top-level Makefile
# This is the entry point for building the entire project

.PHONY: all clean rebuild help

# Default target - build from src directory
all:
	@$(MAKE) -C src -f Master.make

# Clean
clean:
	@$(MAKE) -C src -f Master.make clean

# Rebuild
rebuild:
	@$(MAKE) -C src -f Master.make rebuild

# Help
help:
	@echo "KTPlace Top-level Build System"
	@echo "=============================="
	@echo "Targets:"
	@echo "  all      - Build the project (default)"
	@echo "  clean    - Remove build artifacts"
	@echo "  rebuild  - Clean and rebuild"
	@echo "  help     - Show this help message"
	@echo ""
	@echo "Run 'make help' in src/ for more detailed options."

