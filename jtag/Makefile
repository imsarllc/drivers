DRIVER := xilinx_jtag

ccflags-y := -DLOG_PREFIX=\"$(DRIVER):\ \"
$(DRIVER)-y := \
	  xvc_driver_base.o \
		xvc_driver.o

obj-m += $(DRIVER).o

PWD := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
include $(PWD)/../drivers.mk
