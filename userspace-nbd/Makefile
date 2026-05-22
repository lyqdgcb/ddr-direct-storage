CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -pthread
CPPFLAGS ?= -Iinclude
LDFLAGS ?= -pthread
URMA_ROOT ?= ../external/umdk/src/urma
URMA_LIBDIR ?= $(URMA_ROOT)/build/lib
URMA_LIBS ?= -lurma

ifeq ($(HAVE_URMA),1)
CPPFLAGS += -DHAVE_URMA=1 -I$(URMA_ROOT)/lib/urma/core/include
LDFLAGS += -L$(URMA_LIBDIR) $(URMA_LIBS)
endif

UNAME_S := $(shell uname -s)
BUILD_DIR := build

COMMON_SRCS := src/ramdisk_backend.c src/ramdisk_urma.c src/ramdisk_ctrl_server.c
COMMON_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(COMMON_SRCS))
CTRL_CLIENT_OBJ := $(BUILD_DIR)/src/ramdisk_ctrl_client.o

TESTS := $(BUILD_DIR)/tests/test_backend \
	$(BUILD_DIR)/tests/test_urma_queue \
	$(BUILD_DIR)/tests/test_ctrl_server

ifeq ($(UNAME_S),Linux)
BINARIES := $(BUILD_DIR)/nbd-ramdisk $(BUILD_DIR)/ramdiskctl \
	$(BUILD_DIR)/examples/business_client_example
else
BINARIES := $(BUILD_DIR)/ramdiskctl \
	$(BUILD_DIR)/examples/business_client_example
endif

.PHONY: all test clean integration-help

all: $(BINARIES) $(TESTS)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/nbd-ramdisk: $(COMMON_OBJS) $(BUILD_DIR)/src/nbd_ramdisk.o
	$(CC) $^ $(LDFLAGS) -o $@

$(BUILD_DIR)/ramdiskctl: $(BUILD_DIR)/src/ramdiskctl.o $(CTRL_CLIENT_OBJ)
	$(CC) $^ $(LDFLAGS) -o $@

$(BUILD_DIR)/examples/business_client_example: $(BUILD_DIR)/examples/business_client_example.o $(CTRL_CLIENT_OBJ)
	$(CC) $^ $(LDFLAGS) -o $@

$(BUILD_DIR)/tests/test_backend: $(BUILD_DIR)/tests/test_backend.o $(BUILD_DIR)/src/ramdisk_backend.o
	$(CC) $^ $(LDFLAGS) -o $@

$(BUILD_DIR)/tests/test_urma_queue: $(BUILD_DIR)/tests/test_urma_queue.o $(BUILD_DIR)/src/ramdisk_backend.o $(BUILD_DIR)/src/ramdisk_urma.o
	$(CC) $^ $(LDFLAGS) -o $@

$(BUILD_DIR)/tests/test_ctrl_server: $(BUILD_DIR)/tests/test_ctrl_server.o $(COMMON_OBJS)
	$(CC) $^ $(LDFLAGS) -o $@

test: all
	$(BUILD_DIR)/tests/test_backend
	$(BUILD_DIR)/tests/test_urma_queue
	$(BUILD_DIR)/tests/test_ctrl_server

integration-help:
	@echo "Linux manual test:"
	@echo "  sudo modprobe nbd max_part=8"
	@echo "  sudo ./build/nbd-ramdisk --device /dev/nbd0 --size 128M --enable-urma"
	@echo "  sudo ln -sf /dev/nbd0 /dev/ramdisk"
	@echo "  sudo mkfs.ext4 -F /dev/ramdisk"
	@echo "  sudo mount /dev/ramdisk /mnt/ramdisk"

clean:
	rm -rf $(BUILD_DIR)
