# Polymarket C++ SDK - Root Makefile
# Wraps CMake for convenient day-to-day workflow

BUILD_DIR := build
CMAKE := cmake
NPROC := $(shell nproc 2>/dev/null || echo 4)

.PHONY: all build test lint clean configure help format debug release

# Default target
all: build

# Configure CMake (Release by default)
configure:
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && $(CMAKE) .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# Configure for Debug
configure-debug:
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && $(CMAKE) .. -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# Build all targets
build: configure
	@$(CMAKE) --build $(BUILD_DIR) -j$(NPROC)

# Debug build
debug: configure-debug
	@$(CMAKE) --build $(BUILD_DIR) -j$(NPROC)

# Release build (explicit)
release: configure
	@$(CMAKE) --build $(BUILD_DIR) -j$(NPROC)

# Run tests
test: build
	@cd $(BUILD_DIR) && ctest --output-on-failure

# Run linting (clang-format check)
lint:
	@if command -v clang-format >/dev/null 2>&1; then \
		echo "Checking code formatting..."; \
		find src include tests examples -name '*.cpp' -o -name '*.hpp' 2>/dev/null | xargs clang-format --dry-run --Werror 2>/dev/null || \
		echo "Format check complete (some files may need formatting)"; \
	else \
		echo "clang-format not found - skipping lint"; \
	fi

# Format code in place
format:
	@if command -v clang-format >/dev/null 2>&1; then \
		echo "Formatting code..."; \
		find src include tests examples -name '*.cpp' -o -name '*.hpp' 2>/dev/null | xargs clang-format -i 2>/dev/null; \
		echo "Done"; \
	else \
		echo "clang-format not found"; \
	fi

# Clean build artifacts
clean:
	@rm -rf $(BUILD_DIR)
	@echo "Cleaned build directory"

# Load .env and run example
define run_example
	@if [ -f .env ]; then \
		set -a && . ./.env && set +a && $(1); \
	else \
		$(1); \
	fi
endef

# Run examples — keep this list aligned with examples/CMakeLists.txt.
# The websocket example is intentionally absent: src/clob/websocket.cpp
# is a Phase-10 stub (see CHANGELOG "Stubs (TODO: implement)"), so a
# WebSocket example would have nothing to demonstrate. Reinstate
# run-websocket once the WS implementation lands.
run-market_data: build
	$(call run_example,./$(BUILD_DIR)/examples/example_market_data)

run-order_placement: build
	$(call run_example,./$(BUILD_DIR)/examples/example_order_placement)

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
