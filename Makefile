# Compiler / flags
CXX      := g++
CXXFLAGS := -std=c++20 -O2 -fsanitize=address -g -fno-omit-frame-pointer -Iinclude
LDFLAGS  := -fsanitize=address -pthread

# Layout
SRC_DIR  := src
TEST_DIR := tests
OBJ_DIR  := build
BIN_DIR  := bin

# -------------------------------------------------------------------
# Sources / objects
# -------------------------------------------------------------------
# All library/app sources
SRCS      := $(shell find $(SRC_DIR) -name '*.cpp')
OBJS      := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(SRCS))

# If you have a main.cpp and want to exclude it from tests linking:
MAIN_OBJ        := $(OBJ_DIR)/main.o
CORE_OBJS       := $(filter-out $(MAIN_OBJ),$(OBJS))

# All tests
TEST_SRCS  := $(wildcard $(TEST_DIR)/*.cpp)
TEST_OBJS  := $(patsubst $(TEST_DIR)/%.cpp,$(OBJ_DIR)/tests/%.o,$(TEST_SRCS))
TEST_BINS  := $(patsubst $(TEST_DIR)/%.cpp,$(BIN_DIR)/%,$(TEST_SRCS))

# Don’t run the benchmark as part of "make tests"
RUN_TEST_BINS := $(filter-out $(BIN_DIR)/allocBench,$(TEST_BINS))

# Optional main app (if you have one)
TARGET := $(BIN_DIR)/finalloc

# Dependencies
DEPS := $(OBJS:.o=.d) $(TEST_OBJS:.o=.d)

# -------------------------------------------------------------------
# Default: build everything
# -------------------------------------------------------------------
.PHONY: all
all: $(TARGET) $(TEST_BINS)

# -------------------------------------------------------------------
# Build rules
# -------------------------------------------------------------------
$(BIN_DIR):
	@mkdir -p $(BIN_DIR)

# Core objects from src/
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Test objects from tests/
$(OBJ_DIR)/tests/%.o: $(TEST_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Generic link rule for any test: bin/<name> from tests/<name>.cpp + core objs
$(BIN_DIR)/%: $(OBJ_DIR)/tests/%.o $(CORE_OBJS) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# Optional main app (only if main.cpp exists)
$(TARGET): $(OBJS) | $(BIN_DIR)
	@if [ -f "$(SRC_DIR)/main.cpp" ]; then \
		$(CXX) $(CXXFLAGS) $(OBJS) -o $@ $(LDFLAGS); \
	else \
		echo ">> No src/main.cpp found; skipping $(TARGET)"; \
	fi

# -------------------------------------------------------------------
# Phony helpers
# -------------------------------------------------------------------
.PHONY: tests bench clean

# Build & run all tests except the benchmark
tests: $(RUN_TEST_BINS)
	@set -e; \
	for t in $(notdir $(RUN_TEST_BINS)); do \
		echo "== Running bin/$$t =="; \
		./bin/$$t; \
	done

# Build the benchmark (no default run)
bench: $(BIN_DIR)/allocBench
	@echo "Built $(BIN_DIR)/allocBench"

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)

-include $(DEPS)
