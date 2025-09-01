# ---- toolchain ---------------------------------------------------------------
CXX      := g++
CXXFLAGS := -std=c++20 -O2 -fsanitize=address -g -Iinclude -MMD -MP
LDFLAGS  := -fsanitize=address -pthread

# ---- layout -----------------------------------------------------------------
SRC_DIR  := src
OBJ_DIR  := build
BIN_DIR  := bin
TEST_DIR := tests

# All project sources (recursively)
SRCS := $(shell find $(SRC_DIR) -name '*.cpp')
OBJS := $(SRCS:$(SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

# Optional main (if you have src/main.cpp). If not present, it's fine.
MAIN_OBJ      := $(OBJ_DIR)/main.o
NON_MAIN_OBJS := $(filter-out $(MAIN_OBJ), $(OBJS))

# App target (optional; only if you have a real main)
TARGET := $(BIN_DIR)/finalloc

# ---- tests ------------------------------------------------------------------
TESTS       := allocatorMetricsTest allocatorPerfTest arenaAllocatorTest
TEST_SRCS   := $(addprefix $(TEST_DIR)/,$(addsuffix .cpp,$(TESTS)))
TEST_OBJS   := $(TEST_SRCS:$(TEST_DIR)/%.cpp=$(OBJ_DIR)/tests/%.o)
TEST_BINS   := $(addprefix $(BIN_DIR)/,$(TESTS))
TEST_DEPS   := $(TEST_OBJS:.o=.d)

# ---- default targets ---------------------------------------------------------
.PHONY: all clean test

all: $(TARGET)

# Build the main binary (optional)
$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@ $(LDFLAGS)

# Generic rule for project objects
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# ---- tests build rules -------------------------------------------------------
# Compile each test source to an object
$(OBJ_DIR)/tests/%.o: $(TEST_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Link each test with all non-main objects from src/
$(BIN_DIR)/%: $(OBJ_DIR)/tests/%.o $(NON_MAIN_OBJS) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# Convenience rule to build *and* run all tests
test: $(TEST_BINS)
	@set -e; \
	for t in $(TESTS); do \
		echo "== Running bin/$$t =="; \
		"./$(BIN_DIR)/$$t"; \
		echo; \
	done

# ---- housekeeping ------------------------------------------------------------
$(BIN_DIR):
	@mkdir -p $(BIN_DIR)

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)

# Include auto-generated dep files
-include $(DEPS) $(TEST_DEPS)
