KERNEL_VERSIONS = 2013.4 2016.4

drivers = intc sarspi uio allocated-gpio

default: 2016.4

all: $(KERNEL_VERSIONS)

201%: KVER=$@
201%:	KREL=$(shell cat $(KDIR)/include/config/kernel.release)
201%:
	./version.sh
	$(foreach dir,$(drivers),$(MAKE) -C $(dir) all;)
	mkdir -p build/udev/
	cp */*rules build/udev/
	mkdir -p build/$@/usr/lib/modules/$(KREL)/kernel/imsar
	rsync -a $(KDIR)/arch/arm/boot/uImage build/$@/
	rsync -a $(KDIR)/usr/lib/modules/$(KREL)/kernel build/$@/usr/lib/modules/$(KREL)/
	rsync -a $(KDIR)/usr/lib/modules/$(KREL)/modules.order build/$@/usr/lib/modules/$(KREL)/
	rsync -a $(KDIR)/usr/lib/modules/$(KREL)/modules.builtin build/$@/usr/lib/modules/$(KREL)/
	$(foreach dir,$(drivers),mv $(dir)/*.ko build/$@/usr/lib/modules/$(KREL)/kernel/imsar;)

clean_all:
	$(foreach dir,$(drivers),$(MAKE) -C $(dir) clean;)
	rm -fr build

#for KDIR
include drivers.mk
