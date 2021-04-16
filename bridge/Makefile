obj-m += imsar.o
obj-m += iio_test.o

imsar-objs := bridge.o pcie_int.o pcie_cdev.o

PWD := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

#KDIR:=/lib/modules/$(shell uname -r)/build/
KDIR:=/home/derrickg/src/nvidia/Tegra186_Linux_R32.5.0/Linux_for_Tegra/sources/kernel/kernel-4.9

ARCH=arm64
LOCALVERSION=-tegra-adi 
CROSS_COMPILE=/opt/gcc-linaro-7.3.1-2018.05-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-


test: all
	rsync *.ko xavier1:.
	ssh xavier1 "sudo dmesg -C; sudo rmmod imsar; sudo insmod imsar.ko; dmesg"

all:
	ARCH=$(ARCH) LOCALVERSION=$(LOCALVERSION) CROSS_COMPILE=$(CROSS_COMPILE) $(MAKE) -C $(KDIR) M=$(PWD) modules

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
