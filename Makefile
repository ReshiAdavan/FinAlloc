CXX := g++
CXXFLAGS := -std=c++20 -O2 -fsanitize=address -g -Iinclude

SRC_DIR := src
OBJ_DIR := build
BIN_DIR := bin
TEST_DIR := tests

SRCS := $(shell find $(SRC_DIR) -name '*.cpp')
OBJS := $(SRCS:$(SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

MAIN_OBJ := $(OBJ_DIR)/main.o
NON_MAIN_OBJS := $(filter-out $(MAIN_OBJ), $(OBJS))

TARGET := $(BIN_DIR)/finalloc
TEST_TARGET := $(BIN_DIR)/allocatorPerfTest

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -c $< -o $@

-include $(DEPS)

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)

.PHONY: all clean test

$(TEST_TARGET): $(TEST_DIR)/allocatorPerfTest.cpp $(NON_MAIN_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@

test: $(TEST_TARGET)
	@./$(TEST_TARGET)
