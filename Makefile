KERNEL_VERSIONS = 2013.4 2016.4

drivers = intc sarspi uio allocated-gpio lcd

default: 2016.4

drivers: $(KERNEL_VERSIONS)

2013.4: MDST=usr/lib/modules
2016.4: MDST=lib/modules

201%: export KVER=$@
201%: export KREL=$(shell cat $(KDIR)/include/config/kernel.release)
201%:
	./version.sh
	$(foreach dir,$(drivers),$(MAKE) -C $(dir) all;)
	VIVADO=$@ KDIR=$(KDIR) ./curate_artifacts.sh

clean_all:
	$(foreach dir,$(drivers),$(MAKE) -C $(dir) clean;)
	rm -fr build

#for KDIR
include drivers.mk
