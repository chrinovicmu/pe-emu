MODULE_NAME := pmem


ifneq ($(KERNELRELEASE),)

obj-m += $(MODULE_NAME).o

ccflags-y := \
    -I$(src)/src \
    -DDEBUG \
    -Wall \
    -Wextra \
    -Wno-unused-parameter \
    -Wno-missing-field-initializers

EXTRA_CFLAGS += -I$(PWD)/src
else
KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

SRCDIR := $(PWD)/src

.PHONY: all
all: modules

.PHONY: modules
modules:
	@echo "Building PMEM emulation driver for kernel $(shell uname -r)"
	@echo "Kernel build directory: $(KERNELDIR)"
	@echo "Module source directory: $(SRCDIR)"
	$(MAKE) -C $(KERNELDIR) M=$(SRCDIR) modules
	@echo ""
	@echo "Build complete. Module: $(SRCDIR)/$(MODULE_NAME).ko"
	@echo ""
	@echo "To load: sudo insmod $(SRCDIR)/$(MODULE_NAME).ko"
	@echo "  With params: sudo insmod $(SRCDIR)/$(MODULE_NAME).ko phys_start=0x100000000 pmem_size=268435456"
	@echo "To unload:    sudo rmmod $(MODULE_NAME)"
	@echo "To check logs: dmesg | grep pmem"

.PHONY: install
install: modules
	@echo "Installing pmem.ko to /lib/modules/$(shell uname -r)/extra/"
	sudo cp $(SRCDIR)/$(MODULE_NAME).ko /lib/modules/$(shell uname -r)/extra/
	sudo depmod -a
	@echo "Module installed. Load with: sudo modprobe $(MODULE_NAME)"

.PHONY: uninstall
uninstall:
	sudo rm -f /lib/modules/$(shell uname -r)/extra/$(MODULE_NAME).ko
	sudo depmod -a
	@echo "Module uninstalled."

.PHONY: clean
clean:
	$(MAKE) -C $(KERNELDIR) M=$(SRCDIR) clean
	# Also clean any files kbuild might miss
	find $(SRCDIR) -name "*.o" -delete
	find $(SRCDIR) -name "*.ko" -delete
	find $(SRCDIR) -name "*.mod*" -delete
	find $(SRCDIR) -name ".*.cmd" -delete
	find $(SRCDIR) -name "modules.order" -delete
	find $(SRCDIR) -name "Module.symvers" -delete
	@echo "Clean complete."

.PHONY: load
load: modules
	@echo "Loading pmem module..."
	sudo insmod $(SRCDIR)/$(MODULE_NAME).ko
	@echo "Module loaded. Check: lsmod | grep pmem"
	@echo "Device should appear at: ls -la /dev/pmem0"
	dmesg | tail -20

.PHONY: load-custom
load-custom: modules
	@echo "Loading pmem module with custom parameters..."
	@echo "  phys_start=0x100000000 (4 GiB)"
	@echo "  pmem_size=268435456 (256 MiB)"
	@echo ""
	@echo "PREREQUISITE: Boot kernel with: memmap=256M\!4G"
	@echo "Add to GRUB: GRUB_CMDLINE_LINUX_DEFAULT='memmap=256M\!4G'"
	@echo ""
	sudo insmod $(SRCDIR)/$(MODULE_NAME).ko \
		phys_start=0x100000000 \
		pmem_size=268435456 \
		use_wb_cache=1

.PHONY: unload
unload:
	@echo "Unloading pmem module..."
	sudo rmmod $(MODULE_NAME)
	@echo "Module unloaded."

.PHONY: status
status:
	@echo "=== Module Status ==="
	@lsmod | grep pmem || echo "pmem module not loaded"
	@echo ""
	@echo "=== Device Node ==="
	@ls -la /dev/pmem* 2>/dev/null || echo "No pmem device nodes found"
	@echo ""
	@echo "=== Kernel Messages ==="
	@dmesg | grep -i pmem | tail -30
	@echo ""
	@echo "=== Block Device Info ==="
	@lsblk /dev/pmem0 2>/dev/null || echo "No pmem block device"

.PHONY: test-dax
test-dax:
	@echo "=== DAX Test Setup ==="
	@echo "1. Formatting /dev/pmem0 with ext4..."
	sudo mkfs.ext4 -b 4096 -E lazy_itable_init=0 /dev/pmem0
	@echo ""
	@echo "2. Creating mount point..."
	sudo mkdir -p /mnt/pmem
	@echo ""
	@echo "3. Mounting with DAX support..."
	sudo mount -o dax /dev/pmem0 /mnt/pmem
	@echo ""
	@echo "4. Verifying DAX is active..."
	mount | grep pmem
	@echo ""
	@echo "5. Running simple DAX test..."
	sudo dd if=/dev/zero of=/mnt/pmem/testfile bs=4096 count=1024
	sudo dd if=/mnt/pmem/testfile of=/dev/null bs=4096 count=1024
	@echo ""
	@echo "DAX test complete. /mnt/pmem is ready for use."
	@echo "Unmount with: sudo umount /mnt/pmem"

.PHONY: test
test:
	@echo "Building and running userspace tests..."
	gcc -O2 -Wall -o tests/pmem_test tests/pmem_test.c
	sudo ./tests/pmem_test /dev/pmem0

.PHONY: help
help:
	@echo "PMEM Emulation Driver — Build System"
	@echo "======================================"
	@echo ""
	@echo "Build targets:"
	@echo "  all / modules    Build the kernel module (default)"
	@echo "  clean            Remove all generated files"
	@echo "  install          Install module to system module directory"
	@echo "  uninstall        Remove module from system"
	@echo ""
	@echo "Runtime targets:"
	@echo "  load             Load module with default parameters"
	@echo "  load-custom      Load with phys_start=4GiB pmem_size=256MiB"
	@echo "  unload           Unload the module"
	@echo "  status           Show module/device status and kernel messages"
	@echo ""
	@echo "Testing targets:"
	@echo "  test-dax         Format /dev/pmem0 and mount with DAX"
	@echo "  test             Run userspace test suite"
	@echo ""
	@echo "Prerequisites:"
	@echo "  - Kernel headers: sudo apt install linux-headers-\$$(uname -r)"
	@echo "  - For load-custom: Boot with 'memmap=256M!4G' kernel parameter"
	@echo "    Add to /etc/default/grub: GRUB_CMDLINE_LINUX_DEFAULT='memmap=256M!4G'"
	@echo "    Then: sudo update-grub && sudo reboot"

endif
