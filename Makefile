CXX ?= clang++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2 -Iinclude

SOURCES := \
	src/backtest_engine.cpp \
	src/downloader.cpp \
	src/indicators.cpp \
	src/market_data.cpp \
	src/reporting.cpp \
	src/strategies.cpp

APP_SOURCE := app/main.cpp
TEST_SOURCE := tests/test_main.cpp

BUILD_DIR := build
APP_BIN := $(BUILD_DIR)/nanotrade
TEST_BIN := $(BUILD_DIR)/nanotrade_tests

all: $(APP_BIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(APP_BIN): $(SOURCES) $(APP_SOURCE) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SOURCES) $(APP_SOURCE) -o $(APP_BIN)

$(TEST_BIN): $(SOURCES) $(TEST_SOURCE) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SOURCES) $(TEST_SOURCE) -o $(TEST_BIN)

test: $(TEST_BIN)
	./$(TEST_BIN)

run: $(APP_BIN)
	./$(APP_BIN)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all test run clean
