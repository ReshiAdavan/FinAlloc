CXX := c++
PROFILE ?= asan
BASE_FLAGS := -std=c++20 -g -Wall -Wextra -Werror -pthread -Iinclude
ifeq ($(PROFILE),release)
PROFILE_FLAGS := -O3
SAN_FLAGS :=
else ifeq ($(PROFILE),asan)
PROFILE_FLAGS := -O1 -fno-omit-frame-pointer
SAN_FLAGS := -fsanitize=address,undefined
else ifeq ($(PROFILE),tsan)
PROFILE_FLAGS := -O1 -fno-omit-frame-pointer
SAN_FLAGS := -fsanitize=thread
else ifeq ($(PROFILE),fuzz)
PROFILE_FLAGS := -O1 -fno-omit-frame-pointer
SAN_FLAGS := -fsanitize=fuzzer-no-link,address,undefined
else
$(error Unsupported PROFILE=$(PROFILE))
endif
CXXFLAGS ?= $(BASE_FLAGS) $(PROFILE_FLAGS) $(SAN_FLAGS)
LDFLAGS ?= $(SAN_FLAGS) -pthread
OBJ_DIR := build/$(PROFILE)
BIN_DIR := bin/$(PROFILE)
CORE_SRCS := $(wildcard src/allocators/*.cpp)
CORE_OBJS := $(patsubst src/%.cpp,$(OBJ_DIR)/%.o,$(CORE_SRCS))
TEST_SRCS := $(wildcard tests/*Test.cpp)
TEST_BINS := $(patsubst tests/%.cpp,$(BIN_DIR)/%,$(TEST_SRCS))
DEPS := $(CORE_OBJS:.o=.d) $(OBJ_DIR)/main.d $(OBJ_DIR)/allocBench.d $(patsubst tests/%.cpp,$(OBJ_DIR)/tests/%.d,$(TEST_SRCS))
.DEFAULT_GOAL := all
.PHONY: all test tests bench release asan tsan clean FORCE
all: $(BIN_DIR)/finalloc $(BIN_DIR)/allocBench $(TEST_BINS)
$(OBJ_DIR)/build_info.hpp: FORCE
	@mkdir -p $(OBJ_DIR)
	@python3 tools/build_metadata.py --output $@ --profile $(PROFILE) --compiler '$(CXX)' --flags '$(CXXFLAGS) $(LDFLAGS)'
$(OBJ_DIR)/%.o: src/%.cpp $(OBJ_DIR)/build_info.hpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(OBJ_DIR) -MMD -MP -c $< -o $@
$(OBJ_DIR)/tests/%.o: tests/%.cpp $(OBJ_DIR)/build_info.hpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(OBJ_DIR) -MMD -MP -c $< -o $@
$(OBJ_DIR)/allocBench.o: benchmarks/allocBench.cpp $(OBJ_DIR)/build_info.hpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(OBJ_DIR) -MMD -MP -c $< -o $@
$(BIN_DIR)/%: $(OBJ_DIR)/tests/%.o $(CORE_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)
$(BIN_DIR)/finalloc: $(OBJ_DIR)/main.o $(CORE_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)
$(BIN_DIR)/allocBench: $(OBJ_DIR)/allocBench.o $(CORE_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS) $(BENCH_LIBS)
test tests: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo "Running $$t"; ./$$t; done
bench: $(BIN_DIR)/allocBench
release:
	$(MAKE) PROFILE=release all
asan:
	$(MAKE) PROFILE=asan all test
tsan:
	$(MAKE) PROFILE=tsan bin/tsan/poolConcurrencyTest
	./bin/tsan/poolConcurrencyTest
clean:
	rm -rf build bin
-include $(DEPS)
