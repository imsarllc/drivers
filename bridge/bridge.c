#include "pcie_bridge.h"

#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>

#include <linux/kernel.h>
#include <linux/module.h>

static int setup_axil(struct pci_dev *dev, struct device_node *fpga_node)
{
	u64 taddr;
	u32 bytes[3] = { 0 };
	__be32 *start_addr = (__be32 *)bytes;
	struct device_node *axil_node;
	int rv = 0;

	axil_node = of_get_child_by_name(fpga_node, "axilite");
	if (!axil_node) {
		pr_err("Didn't find axilite child node.  No child devices will be enabled\n");
		return -ENOENT;
	}

	taddr = of_translate_address(axil_node, start_addr);
	if (taddr != pci_resource_start(dev, AXIL_BAR)) {
		pr_err("DT (%llx) is not consistent with actual BAR (%llx).  Update the device tree\n",
		       taddr, pci_resource_start(dev, AXIL_BAR));
		return -EFAULT;
	}

	pr_info("DT is consistent with actual BAR.  Good guess work!\n");

	if ((rv = of_platform_default_populate(fpga_node, NULL, &dev->dev))) {
		pr_err("platform_populate failed\n");
		return rv;
	}
	return 0;
}

static void cleanup_axil(struct pci_dev *dev)
{
	of_platform_depopulate(&dev->dev);
}

static int probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	struct device_node *fpga_node;
	struct imsar_pcie *drvdata;

	pr_info("probe\n");
	if (pci_enable_device(dev) < 0) {
		dev_err(&(dev->dev), "pci_enable_device\n");
		goto error;
	}

	pci_set_master(dev);
	// Only 32-bit DMA capable.
	if (!pci_set_dma_mask(dev, DMA_BIT_MASK(32))) {
		pci_set_consistent_dma_mask(dev, DMA_BIT_MASK(32));
	} else {
		pr_err("No suitable DMA mask\n");
	}

	drvdata = kzalloc(sizeof(struct imsar_pcie), GFP_KERNEL);
	drvdata->pci = dev;
	pci_set_drvdata(dev, drvdata);

	fpga_node = of_find_compatible_node(NULL, NULL, "pci10ee_9024");
	if (fpga_node) {
		dev->dev.of_node = fpga_node;
		imsar_setup_interrupts(dev, fpga_node);
		imsar_setup_nail(dev, fpga_node);
		setup_axil(dev, fpga_node);
	} else {
		pr_err("Didn't find fpga node.  No children enabled\n");
	}

	return 0;
error:
	return -1;
}

static void remove(struct pci_dev *dev)
{
	pr_info("remove\n");

	cleanup_axil(dev);
	imsar_cleanup_nail(dev);
	imsar_cleanup_interrupts(dev);

	pci_clear_master(dev);
	pci_disable_device(dev);
}

#define VENDOR_ID 0x10ee
#define DEVICE_ID 0x9024

static struct pci_device_id id_table[] = {
	{ PCI_DEVICE(VENDOR_ID, DEVICE_ID) },
	{},
};

MODULE_DEVICE_TABLE(pci, id_table);

static struct pci_driver pci_driver = {
	.name = "imsar_pcie",
	.id_table = id_table,
	.probe = probe,
	.remove = remove,
};

static int myinit(void)
{
	if (pci_register_driver(&pci_driver) < 0) {
		return 1;
	}
	return 0;
}

static void myexit(void)
{
	pci_unregister_driver(&pci_driver);
}

module_init(myinit);
module_exit(myexit);

MODULE_LICENSE("GPL");
