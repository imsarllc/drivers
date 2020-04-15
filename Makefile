KERNEL_VERSIONS = 2016.4 2019.2

drivers = intc sarspi uio allocated-gpio lcd jtag

default: 2016.4

drivers: $(KERNEL_VERSIONS)

2013.4: MDST=usr/lib/modules
2016.4: MDST=lib/modules

20%: export KVER=$@
20%: export KREL=$(shell cat $(KDIR)/include/config/kernel.release)
20%:
	./version.sh
	$(foreach dir,$(drivers),$(MAKE) -C $(dir) all;)
	VIVADO=$@ KDIR=$(KDIR) ./curate_artifacts.sh

clean_all:
	$(foreach dir,$(drivers),$(MAKE) -C $(dir) clean;)
	rm -fr build

#for KDIR
include drivers.mk
