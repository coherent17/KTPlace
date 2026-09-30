# KTPlace Top-level Makefile
# This is the entry point for building the entire project

.PHONY: all clean rebuild test check help format format-check clang-format hooks

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

# Rewrite every source file in place with clang-format.
format:
	@$(MAKE) format-check >/dev/null 2>&1 || true
	@find src -type f \( -name '*.cc' -o -name '*.h' \) -print0 | \
		xargs -0 --no-run-if-empty clang-format -i
	@echo "formatted $$(find src -type f \( -name '*.cc' -o -name '*.h' \) | wc -l) file(s)"

# Report what format would change, without touching anything. Exits non-zero if
# anything is unformatted, so it can gate a commit or a CI job.
format-check:
	@find src -type f \( -name '*.cc' -o -name '*.h' \) -print0 | \
		xargs -0 --no-run-if-empty clang-format --dry-run --Werror

# Which clang-format would be used, and what version it is. Worth knowing before
# running format on a tree, since a different major version lays the file out
# differently and produces a diff that is style, not substance.
clang-format:
	@command -v clang-format || { echo "clang-format not found on PATH"; exit 1; }
	@clang-format --version

# Help
# Install the repository's git hooks by pointing core.hooksPath at them, so the
# hook is shared through the tree rather than copied into .git/hooks by hand.
hooks:
	@git config core.hooksPath .githooks
	@echo "core.hooksPath = $$(git config core.hooksPath)"
	@echo "pre-commit now checks staged formatting and runs the unit tests."
	@echo "Bypass with KTPLACE_SKIP_HOOKS=1; skip just the tests with KTPLACE_HOOK_TESTS=0."

help:
	@echo "KTPlace Top-level Build System"
	@echo "=============================="
	@echo "Targets:"
	@echo "  all           - Build the project (default)"
	@echo "  clean         - Remove build artifacts"
	@echo "  rebuild       - Clean and rebuild"
	@echo "  test          - Build and run the unit tests (alias: check)"
	@echo "  format        - Rewrite the sources with clang-format"
	@echo "  format-check  - Report formatting problems, changing nothing"
	@echo "  clang-format  - Show which clang-format would be used"
	@echo "  hooks         - Install the git hooks (pre-commit checks format + tests)"
	@echo "  help          - Show this help message"
	@echo ""
	@echo "Run 'make help' in src/ for more detailed options."

