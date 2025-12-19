# IMSAR Linux Drivers
**IMSAR's public repo containing GPL licensed sources**

## IMSAR Developed Drivers

### allocated-gpio
Expose GPIO pin controls to user space via sysfs

### bridge (imsar_pcie_bridge)
Provide interrupts from a Xilinx AXI Interrupt Controller core
exposed through a Xilinx AXI/PCIe bridge

### imdma, imdma-user
Expose DMA transfers to user space

### imsar-named-gpio
Expose a GPIO input or output to user space as a character device

### imsar-xdma
A circular buffer driver for the Xilinx AXI DMA core

### intc
IMSAR interrupt controller driver

### sarspi
A user space bi-directional SPI driver that uses `read()`/`write()` instead of `ioctl()`

### uio
IMSAR Memory Mapped Device wrapper

### user-interrupt
Expose an interrupt available to the kernel to user space


## Third-Party Drivers

### dtbocfg
A copy of the Device Tree Blob Overlay Configuration File System module

From https://github.com/ikwzm/dtbocfg

### lcd
Fork of LCD driver for Newhaven NHD-0216K3Z-NSW-BBW from https://github.com/enclustra-bsp/altera-linux/blob/master/drivers/tty/newhaven_lcd.c


### Analog Devices
The drivers in the `adi` folder come from Analog Device's linux tree.
They are modified to support a standard 5.15 kernel.
The Analog Devices drivers are all licensed under GPL-2.0.

- ad9172
- ad9208
- axi_adxcvr
- axi_jesd204_rx
- axi_jesd204_tx
- cf_axi_adc
- hmc7044
- jesd204

### Xilinx Drivers

#### From Xilinx Linux kernel tree
The drivers in the `xilinx` folder come from Xilinx's linux tree.
The Xilinx drivers are all licensed under GPL-2.0.

- gpio-xilinx
- i2c-xiic
- spi-xilinx
- xilinx-xadc (modified slightly to append a name to each channel)


#### From Xilinx GitHub

### jtag
This is a fork of the Xilinx XVC driver (https://github.com/Xilinx/XilinxVirtualCable) with slight modifications