CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS += -Iinclude
WARNFLAGS := -Wall -Wextra -Wpedantic -Werror
STD := -std=gnu11

BUILD_DIR := build
LIB_OBJS := \
	$(BUILD_DIR)/rdma_ramdisk.o \
	$(BUILD_DIR)/target_session.o \
	$(BUILD_DIR)/mock_rdma.o

TEST_BIN := $(BUILD_DIR)/test_rdmadisk

.PHONY: all test clean run

all: $(TEST_BIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(STD) $(CPPFLAGS) $(CFLAGS) $(WARNFLAGS) -c $< -o $@

$(BUILD_DIR)/test_rdmadisk.o: tests/test_rdmadisk.c | $(BUILD_DIR)
	$(CC) $(STD) $(CPPFLAGS) $(CFLAGS) $(WARNFLAGS) -c $< -o $@

$(TEST_BIN): $(LIB_OBJS) $(BUILD_DIR)/test_rdmadisk.o
	$(CC) $(CFLAGS) $^ -o $@

test: $(TEST_BIN)
	$(TEST_BIN)

run: test

clean:
	rm -rf $(BUILD_DIR)
