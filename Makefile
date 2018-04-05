KERNEL_VERSIONS = 2013.4 2016.4

drivers = intc sarspi uio allocated-gpio

default: 2016.4

drivers: $(KERNEL_VERSIONS)

2013.4: MDST=usr/lib/modules
2016.4: MDST=lib/modules

201%: export KVER=$@
201%:	export KREL=$(shell cat $(KDIR)/include/config/kernel.release)
201%:
	./version.sh
	$(foreach dir,$(drivers),$(MAKE) -C $(dir) all;)
	mkdir -p build/udev/
	cp */*rules build/udev/
	mkdir -p build/$@/$(MDST)/$(KREL)/kernel/drivers/imsar
	rsync -a $(KDIR)/arch/arm/boot/uImage build/$@/
	rsync -a $(KDIR)/usr/lib/modules/$(KREL)/kernel          build/$@/$(MDST)/$(KREL)/
	rsync -a $(KDIR)/usr/lib/modules/$(KREL)/modules.order   build/$@/$(MDST)/$(KREL)/
	rsync -a $(KDIR)/usr/lib/modules/$(KREL)/modules.builtin build/$@/$(MDST)/$(KREL)/
	mv $(foreach dir,$(drivers), $(dir)/*.ko)                build/$@/$(MDST)/$(KREL)/kernel/drivers/imsar

clean_all:
	$(foreach dir,$(drivers),$(MAKE) -C $(dir) clean;)
	rm -fr build

#for KDIR
include drivers.mk
