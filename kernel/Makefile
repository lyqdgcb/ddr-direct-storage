# RDMA Ramdisk Block Device Driver
#
# Build:
#   make               - Build the kernel module
#   make clean         - Clean build artifacts
#   make install       - Install module (requires root)
#   make uninstall     - Uninstall module (requires root)
#
# Usage:
#   insmod rdma_blkdev.ko device_size_mb=1024 ib_dev_name=mlx5_0
#

obj-m := rdma_blkdev.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

ccflags-y := -I$(PWD)/include

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f Module.symvers modules.order

install: modules
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

uninstall:
	rm -f /lib/modules/$(shell uname -r)/extra/rdma_blkdev.ko
	depmod -a

.PHONY: all modules clean install uninstall