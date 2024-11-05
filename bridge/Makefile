obj-m += imsar_pcie_interrupt_cdev.o

imsar_pcie-objs := imsar_pcie_bridge.o imsar_pcie_int.o imsar_pcie_cdev.o
obj-m += imsar_pcie.o

PWD := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules KBUILD_EXTRA_SYMBOLS=$(KDIR)/Module.symvers

clean:
	rm *.ko *.mod.* *.o
