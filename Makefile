# Polymarket C++ SDK - Root Makefile
# Wraps CMake for convenient day-to-day workflow

BUILD_DIR ?= build
CMAKE_ARGS ?=
CMAKE := cmake
NPROC := $(shell nproc 2>/dev/null || echo 4)

.PHONY: all build test lint clean configure help format pre-commit install-hooks debug release consumers configure-debug

# Default target
all: build

# Configure CMake (Release by default)
configure:
	@$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON $(CMAKE_ARGS)
configure-debug:
	@$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON $(CMAKE_ARGS)

# Build all targets
build: configure
	@$(CMAKE) --build $(BUILD_DIR) -j$(NPROC)

# Debug uses a separate directory.
debug:
	$(MAKE) build BUILD_DIR=build-debug CMAKE_ARGS="-DCMAKE_BUILD_TYPE=Debug $(CMAKE_ARGS)"

# Release build (explicit)
release: configure
	@$(CMAKE) --build $(BUILD_DIR) -j$(NPROC)

# Run tests
test: build
	@cd $(BUILD_DIR) && ctest --output-on-failure

SOURCES := $(shell find src include tests examples apps -type f \( -name '*.cpp' -o -name '*.hpp' \))
lint:
	clang-format --dry-run --Werror $(SOURCES)
format:
	clang-format -i $(SOURCES)

# pre-commit: auto-format, then lint. Run before every commit to avoid
# the recurring "push -> CI clang-format fail -> follow-up fix PR" loop.
# Idempotent — `format` re-running is a no-op.
pre-commit: format lint

# Install a non-mutating hook without replacing unrelated hooks.
install-hooks:
	@hook="$$(git rev-parse --git-path hooks)/pre-commit"; \
	if [ -f "$$hook" ]; then \
        case "$$(cat "$$hook")" in \
            "$$(printf '#!/bin/sh\nexec make lint')"|"$$(printf '#!/bin/sh\nexec make pre-commit')") ;; \
            *) echo "$$hook already exists; not replacing it"; exit 1 ;; \
        esac; \
    fi; \
	mkdir -p "$$(dirname "$$hook")" && printf '#!/bin/sh\nexec make lint\n' > "$$hook" && \
	chmod +x "$$hook" && echo "Installed $$hook (runs make lint)"


# Clean build artifacts
clean:
	@rm -rf $(BUILD_DIR) build-debug
	@echo "Cleaned build directory"

# Load .env and run example
define run_example
	@if [ -f .env ]; then \
		set -a && . ./.env && set +a && $(1); \
	else \
		$(1); \
	fi
endef

# Example targets match examples/CMakeLists.txt.
run-market_data: build
	$(call run_example,./$(BUILD_DIR)/examples/example_market_data)

run-order_placement: build
	$(call run_example,./$(BUILD_DIR)/examples/example_order_placement)

run-us_smoke: build
	$(call run_example,./$(BUILD_DIR)/examples/example_us_smoke)

# Help
help:
	@echo "Polymarket C++ SDK Build System"
	@echo ""
	@echo "Targets:"
	@echo "  make build          - Configure and build the SDK (Release)"
	@echo "  make debug          - Build in Debug mode"
	@echo "  make release        - Build in Release mode"
	@echo "  make test           - Run tests"
	@echo "  make lint           - Check code formatting"
	@echo "  make format         - Format code in place"
	@echo "  make clean          - Remove build artifacts"
	@echo "  make help           - Show this help"
	@echo ""
	@echo "Examples:"
	@echo "  make run-market_data      - Run market data example"
	@echo "  make run-order_placement  - Run order placement example"
	@echo "  make run-us_smoke         - Smoke-test the US client (auth + 5 endpoints)"

consumers:
	bash tools/test_consumers.sh $(CMAKE_ARGS)
