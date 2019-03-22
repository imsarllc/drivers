#gcc 4.8 is used on Ubuntu 2014.04 (LTS), which is used on the Tegra TK1.
CROSS_COMPILE?=/opt/gcc/gcc-linaro-4.8-2014.4-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-

KVER ?= 2016.4
KBASE ?= /fpga_tools/Xilinx/kernel
KDIR = $(KBASE)/imsar-v$(KVER)

all: ../version.h
	cp ../version.h .
	@$(MAKE) -C $(KDIR) M=$(PWD) ARCH=arm CROSS_COMPILE=$(CROSS_COMPILE) modules
	rm version.h

clean:
	@$(RM) version.h
	@$(MAKE) -C $(KDIR) M=$(PWD) clean

../version.h:
	../version.sh ..
