#include "imsar_pcie_bridge.h"

#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>

#include <linux/kernel.h>
#include <linux/module.h>

static int imsar_pcie_check_addresses(struct pci_dev *dev, struct device_node *fpga_node)
{
	u64 dt_addr;
	u64 act_addr;
	// __be32 start_addr;
	u32 bytes[3] = { 0 };
	__be32 *start_addr = (__be32 *)bytes;

	struct device_node *fpga_child_node;
	struct device_node *fpga_grandchild_node;

	int rv = 0;
	int bar_index_res = -1;
	u32 bar_index = 0;

	// Iterate through each child node looking for compatible="simple-bus". On each match:
	// - Find its first child with compatible="imsar,address-check"
	// - Retrieve PCI bar index attribute
	// - Translate the node's address with offset 0 using DT
	// - Retrieve PCI BAR address
	// - Compare and flag error if mismatched

	// Iterate through each child node
	for_each_child_of_node (fpga_node, fpga_child_node) {
		// Skip if not a simple-bus
		if (!of_device_is_compatible(fpga_child_node, "simple-bus")) {
			continue;
		}

		// Iterate through each grandchild node
		for_each_child_of_node (fpga_child_node, fpga_grandchild_node) {
			// Skip if the node is not an address check node
			if (!of_device_is_compatible(fpga_grandchild_node, "imsar,address-check")) {
				continue;
			}

			// Read imsar,bar-index property
			bar_index_res = of_property_read_u32(fpga_grandchild_node,
							     "imsar,bar-index", &bar_index);

			// Set code to error and skip
			if (bar_index_res < 0) {
				dev_err(&dev->dev,
					"%pOF is missing required imsar,bar-index. Please fix device tree.",
					fpga_grandchild_node);
				rv = -EINVAL;
				continue;
			}

			// Translate using DT
			dt_addr = of_translate_address(fpga_grandchild_node, start_addr);

			// Retrieve actual address from PCI
			act_addr = pci_resource_start(dev, bar_index);

			// Check for match
			if (dt_addr != act_addr) {
				dev_err(&dev->dev,
					"DT address for %pOF (%llx) is not consistent with actual BAR (%llx). Update the device tree\n",
					fpga_grandchild_node, dt_addr, act_addr);
				rv = -EFAULT;
			} else {
				dev_info(
					&dev->dev,
					"DT address for %pOF is consistent with actual BAR.  Good guess work!\n",
					fpga_grandchild_node);
			}
		}
	}

	return rv;
}

static int imsar_pcie_setup_simple_buses(struct pci_dev *dev, struct device_node *fpga_node)
{
	int rv = 0;

	rv = imsar_pcie_check_addresses(dev, fpga_node);
	if (rv) {
		dev_err(&dev->dev, "address check failed\n");
		return rv;
	}

	rv = of_platform_default_populate(fpga_node, NULL, &dev->dev);
	if (rv) {
		dev_err(&dev->dev, "platform_populate failed\n");
		return rv;
	}

	return 0;
}

static void imsar_pcie_cleanup_simple_buses(struct pci_dev *dev)
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
		imsar_pcie_setup_interrupts(dev, fpga_node);
		imsar_pcie_setup_simple_buses(dev, fpga_node);
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

	imsar_pcie_cleanup_simple_buses(dev);
	imsar_pcie_cleanup_interrupts(dev);

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
