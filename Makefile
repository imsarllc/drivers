
KERNEL_VERSIONS = 2013.4 2016.4

drivers = intc sarspi uio

default: 2016.4

all: $(KERNEL_VERSIONS)

201%: KVER=$@
201%:
	./version.sh
	$(foreach dir,$(drivers),$(MAKE) -C $(dir) all;)
	mkdir -p build/$@
	$(foreach dir,$(drivers),mv $(dir)/*.ko build/$@;)
	$(foreach dir,$(drivers),cp $(dir)/*rules build/$@;)

clean:
	$(foreach dir,$(drivers),$(MAKE) -C $(dir) clean;)
	rm -r build
