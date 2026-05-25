CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -pthread
CPPFLAGS ?= -Iinclude
LDFLAGS ?= -pthread
URMA_LIBDIR ?= /usr/lib64
URMA_LIBS ?= -lurma
HAVE_URMA ?= 1

ifeq ($(HAVE_URMA),1)
CPPFLAGS += -DHAVE_URMA=1 -I /usr/include/umdk
LDFLAGS += -L$(URMA_LIBDIR) $(URMA_LIBS)
endif

UNAME_S := $(shell uname -s)
BUILD_DIR := build
LIB_DIR := $(BUILD_DIR)/lib

COMMON_SRCS := src/ramdisk_backend.c src/ramdisk_urma.c src/ramdisk_ctrl_server.c
COMMON_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(COMMON_SRCS))
CTRL_CLIENT_OBJ := $(BUILD_DIR)/src/ramdisk_ctrl_client.o
CTRL_CLIENT_PIC_OBJ := $(BUILD_DIR)/pic/src/ramdisk_ctrl_client.o
CTRL_CLIENT_SO := $(LIB_DIR)/libnbd_ramdisk_ctrl.so
CTRL_CLIENT_LIBS := -L$(LIB_DIR) -lnbd_ramdisk_ctrl

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

all: $(BINARIES) $(TESTS) $(CTRL_CLIENT_SO)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/pic/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -c $< -o $@

$(CTRL_CLIENT_SO): $(CTRL_CLIENT_PIC_OBJ)
	@mkdir -p $(dir $@)
	$(CC) -shared -Wl,-soname,libnbd_ramdisk_ctrl.so $^ -o $@

$(BUILD_DIR)/nbd-ramdisk: $(COMMON_OBJS) $(BUILD_DIR)/src/nbd_ramdisk.o
	$(CC) $^ $(LDFLAGS) -o $@

$(BUILD_DIR)/ramdiskctl: $(BUILD_DIR)/src/ramdiskctl.o $(CTRL_CLIENT_SO)
	$(CC) $(BUILD_DIR)/src/ramdiskctl.o $(LDFLAGS) $(CTRL_CLIENT_LIBS) -Wl,-rpath,'$$ORIGIN/lib' -o $@

$(BUILD_DIR)/examples/business_client_example: $(BUILD_DIR)/examples/business_client_example.o $(CTRL_CLIENT_SO)
	$(CC) $(BUILD_DIR)/examples/business_client_example.o $(LDFLAGS) $(CTRL_CLIENT_LIBS) -Wl,-rpath,'$$ORIGIN/../lib' -o $@

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
