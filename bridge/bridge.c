#include "pcie_bridge.h"

#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>

#include <linux/kernel.h>
#include <linux/module.h>

static int imsar_pcie_setup_axil(struct pci_dev *dev, struct device_node *fpga_node)
{
	u64 dt_addr;
	u64 act_addr;
	// __be32 start_addr;
	u32 bytes[3] = { 0 };
	__be32 *start_addr = (__be32 *)bytes;
	struct device_node *axil_node;
	int rv = 0;

	axil_node = of_get_child_by_name(fpga_node, "axilite");
	if (!axil_node) {
		dev_err(&dev->dev,
			"Didn't find axilite child node.  No child devices will be enabled\n");
		return -ENOENT;
	}

	dt_addr = of_translate_address(axil_node, start_addr);
	act_addr = pci_resource_start(dev, AXIL_BAR);
	if (dt_addr != act_addr) {
		dev_err(&dev->dev,
			"DT (%llx) is not consistent with actual BAR (%llx). Update the device tree\n",
			dt_addr, act_addr);
		return -EFAULT;
	}

	dev_info(&dev->dev, "DT is consistent with actual BAR.  Good guess work!\n");

	rv = of_platform_default_populate(fpga_node, NULL, &dev->dev);
	if (rv) {
		dev_err(&dev->dev, "platform_populate failed\n");
		return rv;
	}

	return 0;
}

static void imsar_pcie_cleanup_axil(struct pci_dev *dev)
{
	of_platform_depopulate(&dev->dev);
}

static int imsar_pcie_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	struct imsar_pcie *drvdata;
	struct device_node *fpga_node;

	dev_info(&dev->dev, "probe\n");

	if (pci_enable_device(dev) < 0) {
		dev_err(&(dev->dev), "pci_enable_device\n");
		goto error;
	}

	pci_set_master(dev);
	// Only 32-bit DMA capable.
	if (!pci_set_dma_mask(dev, DMA_BIT_MASK(32))) {
		pci_set_consistent_dma_mask(dev, DMA_BIT_MASK(32));
	} else {
		dev_err(&dev->dev, "No suitable DMA mask\n");
	}

	drvdata = devm_kzalloc(&dev->dev, sizeof(struct imsar_pcie), GFP_KERNEL);
	drvdata->pci = dev;
	pci_set_drvdata(dev, drvdata);

	fpga_node = of_find_compatible_node(NULL, NULL, "pci10ee_9034");
	if (fpga_node) {
		dev->dev.of_node = fpga_node;
		imsar_pcie_setup_nail(dev, fpga_node);
		imsar_pcie_setup_axil(dev, fpga_node);
		// 	// Interrupt controller is on the axi-lite bus, but
		// 	// axil devices need interrupts enabled.
		// 	imsar_pcie_setup_interrupts(dev, fpga_node);
	} else {
		dev_err(&dev->dev, "Didn't find fpga node.  No children enabled\n");
	}

	return 0;
error:
	return -1;
}

static void imsar_pcie_remove(struct pci_dev *dev)
{
	dev_info(&dev->dev, "remove\n");

	// imsar_pcie_cleanup_interrupts(dev);
	imsar_pcie_cleanup_axil(dev);
	imsar_pcie_cleanup_nail(dev);

	pci_clear_master(dev);
	pci_disable_device(dev);
}

static struct pci_device_id imsar_pcie_id_table[] = {
	{ PCI_DEVICE(0x10ee, 0x9034) },
	{},
};

MODULE_DEVICE_TABLE(pci, imsar_pcie_id_table);

static struct pci_driver pci_driver = {
	.name = "imsar_pcie",
	.id_table = imsar_pcie_id_table,
	.probe = imsar_pcie_probe,
	.remove = imsar_pcie_remove,
};

module_pci_driver(pci_driver);

MODULE_LICENSE("GPL");
