CXX := g++
CXXFLAGS := -std=c++20 -O2 -fsanitize=address -g -Iinclude -fno-omit-frame-pointer
LDFLAGS := -fsanitize=address -pthread

SRC_DIR := src
OBJ_DIR := build
BIN_DIR := bin
TEST_DIR := tests

# Discover all source files under src/
SRCS := $(shell find $(SRC_DIR) -name '*.cpp')
OBJS := $(SRCS:$(SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

# If you have a main.cpp you want to build as the primary binary:
MAIN_OBJ := $(OBJ_DIR)/main.o
NON_MAIN_OBJS := $(filter-out $(MAIN_OBJ), $(OBJS))

TARGET := $(BIN_DIR)/finalloc

# Tests and benchmarks
TEST_BINS := \
  $(BIN_DIR)/allocatorMetricsTest \
  $(BIN_DIR)/allocatorPerfTest \
  $(BIN_DIR)/arenaAllocatorTest

BENCH_TARGET := $(BIN_DIR)/allocBench

# Build everything by default (app + benchmark)
all: $(TARGET) $(BENCH_TARGET)

# Primary app (only if you have a main.cpp; otherwise remove this target)
$(TARGET): $(OBJS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@ $(LDFLAGS)

# Generic compile rule
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# -----------------------------
# Tests
# -----------------------------
$(BIN_DIR)/allocatorMetricsTest): $(TEST_DIR)/allocatorMetricsTest.cpp $(NON_MAIN_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/allocatorPerfTest): $(TEST_DIR)/allocatorPerfTest.cpp $(NON_MAIN_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/arenaAllocatorTest): $(TEST_DIR)/arenaAllocatorTest.cpp $(NON_MAIN_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

tests: $(TEST_BINS)

test: tests
	@set -e; \
	for t in $(notdir $(TEST_BINS)); do \
	  echo "== Running bin/$$t =="; \
	  "./bin/$$t"; \
	  echo ""; \
	done

# -----------------------------
# Benchmark
# -----------------------------
$(BENCH_TARGET): $(TEST_DIR)/allocBench.cpp $(NON_MAIN_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

bench: $(BENCH_TARGET)
	@echo "Built $(BENCH_TARGET). Example:"
	@echo "./bin/allocBench --allocator=pool --threads=8 --iters=100000 --size=64"

# -----------------------------
# Housekeeping
# -----------------------------
clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)

-include $(DEPS)

.PHONY: all tests test bench clean
