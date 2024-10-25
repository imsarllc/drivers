# obj-m += iio_test.o
obj-m += imsar_pcie_interrupt_cdev.o
# obj-m += dma/dma-axi-dmac.o

imsar_pcie-objs := imsar_pcie_bridge.o imsar_pcie_int.o imsar_pcie_cdev.o
obj-m += imsar_pcie.o

# xilinx-xadc-y := xilinx-xadc-core.o xilinx-xadc-events.o
# obj-m += xilinx-xadc.o

PWD := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

#KDIR:=/lib/modules/$(shell uname -r)/build/

KDIR:=/home/derrickg/src/nvidia/Tegra186_Linux_R32.5.0/Linux_for_Tegra/sources/kernel/kernel-4.9
LOCALVERSION=-tegra-adi
#KDIR:=/home/derrickg/src/nvidia/Jetson_Linux_R32.6.1/Linux_for_Tegra/sources/kernel/kernel-4.9
#LOCALVERSION=-tegra

ARCH=arm64
CROSS_COMPILE=/opt/gcc-linaro-7.3.1-2018.05-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-


all:
	ARCH=$(ARCH) LOCALVERSION=$(LOCALVERSION) CROSS_COMPILE=$(CROSS_COMPILE) $(MAKE) -C $(KDIR) M=$(PWD) modules KBUILD_EXTRA_SYMBOLS=$(KDIR)/Module.symvers

upload: all
	rsync --progress *.ko xavier1:.

test: upload
	ssh xavier1 "sudo dmesg -C; sudo rmmod imsar; sudo insmod imsar.ko; dmesg"
	#ssh xavier1 "sudo dmesg -C; sudo rmmod iio_test; sudo insmod iio_test.ko; dmesg"

deps: all
	python .vscode/generate_compdb.py -O $(KDIR) $(PWD)

unload:
	-sudo rmmod dmac
	-sudo rmmod ep

clear:
	sudo dmesg -C

load: clear
	sudo insmod ep.ko
	#sudo insmod dmac.ko
	dmesg

reload: all clear unload load

reload_dma: all clear
	-sudo rmmod dmac
	sudo insmod dmac.ko
	dmesg

load_%: clear
	sudo insmod $*.ko
	dmesg

reload_%: all
	-sudo rmmod $*
	sudo dmesg -C
	sudo insmod $*.ko
	dmesg

clean:
	rm *.ko *.mod.* *.o
