obj-m := jesd204.o
obj-m += cf_axi_adc.o
obj-m += axi_adxcvr.o
obj-m += hmc7044.o
obj-m += ad9208.o
obj-m += ad9172.o
obj-m += axi_jesd204_rx.o
obj-m += axi_jesd204_tx.o

jesd204-y := \
	src/jesd204/jesd204-fsm.o \
	src/jesd204/jesd204-sysfs.o \
	src/jesd204/jesd204-core.o

cf_axi_adc-y := src/cf_axi_adc/cf_axi_adc_core.o

hmc7044-objs := src/hmc7044/hmc7044.o

ad9208-objs := \
	src/ad9208/ad9208/ad9208_adc_api.o \
	src/ad9208/ad9208/ad9208_api.o \
	src/ad9208/ad9208/ad9208_jesd_api.o \
	src/ad9208/ad9208/ad9208_reg.o \
	src/ad9208/ad9208/ad9208_signal_monitor_api.o \
	src/ad9208/ad9208.o

ad9172-objs := \
	src/ad9172/ad917x/ad917x_api.o \
	src/ad9172/ad917x/ad917x_jesd_api.o \
	src/ad9172/ad917x/ad917x_nco_api.o \
	src/ad9172/ad917x/ad917x_reg.o \
	src/ad9172/ad9172.o

axi_adxcvr-objs := \
	src/axi_adxcvr/axi_adxcvr.o \
	src/axi_adxcvr/xilinx_transceiver.o \
	src/axi_adxcvr/axi_adxcvr_eyescan.o

axi_jesd204_rx-objs := \
	src/axi_jesd204_rx/axi_jesd204_rx.o

axi_jesd204_tx-objs := \
	src/axi_jesd204_tx/axi_jesd204_tx.o

ccflags-y := -I$(src)/include
ccflags-y += -DCONFIG_JESD204 # indicate JESD204 is enabled to avoid changing sources
# ccflags-y += -DMICROHZ_PER_HZ=1000000UL # fix because it is not available until kernel 6.1 (used for main)