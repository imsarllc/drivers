#include "imsar_pcie_bridge.h"

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/log2.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/of_address.h>

struct imsar_pcie_intc_info {
	void __iomem *pcie_baseaddr;
	void __iomem *x_baseaddr;
	uint32_t edge_mask;
	struct irq_domain *domain;
	struct pci_dev *pci_dev;
};

/* No one else should require these constants, so define them locally here. */
#define IDR 0x2000 /* Interrupt ID/Version Register */
#define VER 0x2002 /* Version register */
#define IER 0x2004 /* Interrupt Enable Register */
#define SIE 0x2008 /* Set Interrupt Enable bits */
#define CIE 0x200c /* Clear Interrupt Enable bits */
#define ISR 0x2040 /* Interrupt Status Register */
#define IPR 0x2048 /* Interrupt Pending Register */
#define IVM 0x2080 /* Interrupt Vector Map start address */

#define X_ISR 0x00 /* Interrupt Status Register */
#define X_IPR 0x04 /* Interrupt Pending Register */
#define X_IER 0x08 /* Interrupt Enable Register */
#define X_IAR 0x0c /* Interrupt Acknowledge Register */
#define X_SIE 0x10 /* Set Interrupt Enable bits */
#define X_CIE 0x14 /* Clear Interrupt Enable bits */
#define X_IVR 0x18 /* Interrupt Vector Register */
#define X_MER 0x1c /* Master Enable Register */
#define X_MER_ME (1 << 0)
#define X_MER_HIE (1 << 1)

static void imsar_pcie_int_axi_enable_or_unmask(struct irq_data *data)
{
	unsigned long mask = 1 << data->hwirq;
	struct imsar_pcie_intc_info *local_intc = irq_data_get_irq_chip_data(data);

	dev_dbg(&local_intc->pci_dev->dev, "Enable irq=%d, hw_irq=%lu\n", data->irq, data->hwirq);

	// iowrite16(mask, local_intc->baseaddr + SIE);
	/*
	 * ack level irqs because they can't be acked during
	 * ack function since the handle_level_irq function
	 * acks the irq before calling the interrupt handler
	 */
	if (irqd_is_level_type(data)) {
		iowrite32(mask, local_intc->x_baseaddr + X_IAR);
	}

	iowrite32(mask, local_intc->x_baseaddr + X_SIE);
}

static void imsar_pcie_int_axi_disable_or_mask(struct irq_data *data)
{
	unsigned long mask = 1 << data->hwirq;

	struct imsar_pcie_intc_info *local_intc = irq_data_get_irq_chip_data(data);

	dev_dbg(&local_intc->pci_dev->dev, "Disable irq=%d, hw_irq=%lu\n", data->irq, data->hwirq);

	iowrite32(mask, local_intc->x_baseaddr + X_CIE);
}

static void imsar_pcie_int_axi_ack(struct irq_data *data)
{
	struct imsar_pcie_intc_info *local_intc = irq_data_get_irq_chip_data(data);
	iowrite32(1 << data->hwirq, local_intc->x_baseaddr + X_IAR);
}

static void imsar_pcie_int_axi_mask_ack(struct irq_data *data)
{
	unsigned long mask = 1 << data->hwirq;
	struct imsar_pcie_intc_info *local_intc = irq_data_get_irq_chip_data(data);
	iowrite32(mask, local_intc->x_baseaddr + X_CIE);
	iowrite32(mask, local_intc->x_baseaddr + X_IAR);
}

static struct irq_chip intc_dev = {
	.name = "msi-bridge",
	.irq_enable = imsar_pcie_int_axi_enable_or_unmask,
	.irq_unmask = imsar_pcie_int_axi_enable_or_unmask,
	.irq_disable = imsar_pcie_int_axi_disable_or_mask,
	.irq_mask = imsar_pcie_int_axi_disable_or_mask,
	.irq_ack = imsar_pcie_int_axi_ack,
	.irq_mask_ack = imsar_pcie_int_axi_mask_ack,
};

static unsigned int imsar_pcie_int_get_irq(struct imsar_pcie_intc_info *local_intc)
{
	int irq = 0;
	unsigned int hwirq;
	unsigned int mask;

	mask = ioread32(local_intc->x_baseaddr + X_IPR);
	hwirq = ilog2(mask);
	if (hwirq != -1U) {
		irq = irq_find_mapping(local_intc->domain, hwirq);
	}

	// pr_err("imsar_pcie_int_get_irq: hwirq=%d, irq=%d\n", hwirq, irq);
	return irq;
}

static int imsar_pcie_int_xintc_map(struct irq_domain *d, unsigned int irq, irq_hw_number_t hw)
{
	struct imsar_pcie_intc_info *local_intc = d->host_data;

	dev_dbg(&local_intc->pci_dev->dev,
		"Map IRQ: domain = %s, irq = %d, hw_irq=%ld, host_data %p\n", //
		d->name, irq, hw, d->host_data);

	// This should be set by the consumer, not the IRQ chip...
	local_intc->edge_mask = 0xFFFFFFFF;
	if (local_intc->edge_mask & (1 << hw)) {
		irq_set_chip_and_handler_name(irq, &intc_dev, handle_edge_irq, "edge");
		irq_clear_status_flags(irq, IRQ_LEVEL);
		irq_set_irq_type(irq, IRQ_TYPE_EDGE_RISING);
	} else {
		irq_set_chip_and_handler_name(irq, &intc_dev, handle_level_irq, "level");
		irq_set_status_flags(irq, IRQ_LEVEL);
		irq_set_irq_type(irq, IRQ_TYPE_LEVEL_HIGH);
	}

	irq_set_chip_data(irq, local_intc);
	return 0;
}

struct irq_domain_ops imsar_pcie_intc_chip_ops = {
	.xlate = irq_domain_xlate_onecell,
	.map = imsar_pcie_int_xintc_map,
};

static void imsar_pcie_int_irq_flow_handler(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct imsar_pcie_intc_info *local_intc = irq_data_get_irq_handler_data(&desc->irq_data);

	int irq;
	//pr_info("irq_handler called for %s: %d", desc->name, desc->irq_data.irq);

	chained_irq_enter(chip, desc);

	/*
	 * ignore the irq input as we now need to query the AXI interrupt
	 * controller to see which interrupt is active
	 */
	irq = imsar_pcie_int_get_irq(local_intc);
	while (irq) {
		generic_handle_irq(irq);
		irq = imsar_pcie_int_get_irq(local_intc);
	}

	chained_irq_exit(chip, desc);
}

int imsar_pcie_setup_interrupts(struct pci_dev *dev, struct device_node *fpga_node)
{
	u16 id;
	u16 version;
	int rv = 0;
	u32 vec;
	struct imsar_pcie_intc_info *intc_info;
	struct device_node *axi_intc_node;
	struct imsar_pcie *drv_data = (struct imsar_pcie *)pci_get_drvdata(dev);

	intc_info = devm_kzalloc(&dev->dev, sizeof(struct imsar_pcie_intc_info), GFP_KERNEL);
	if (!intc_info) {
		return -ENOMEM;
	}

	intc_info->pci_dev = dev;
	drv_data->intc_info = intc_info;

	axi_intc_node = of_find_compatible_node(NULL, NULL, "imsar,pcie_axi_intc");
	if (!axi_intc_node) {
		dev_err(&dev->dev,
			"Didn't find axi intc expander child node. Interrupts will be disabled\n");
		return -ENOENT;
	}

	intc_info->x_baseaddr = of_iomap(axi_intc_node, 0);
	if (!intc_info->x_baseaddr) {
		dev_err(&dev->dev, "Unable to map memory for axi intc expander\n");
		return -ENOMEM;
	}

	if (pci_request_region(dev, INT_BAR, "bar3_msi_int")) {
		dev_err(&dev->dev, "pci_request_region failed\n");
		return -ENOMEM;
	}

	intc_info->pcie_baseaddr = pci_iomap(dev, INT_BAR, pci_resource_len(dev, INT_BAR));
	if (!intc_info->pcie_baseaddr) {
		dev_err(&dev->dev, "pci_iomap failed\n");
		return -ENOMEM;
	}

	version = ioread16(intc_info->pcie_baseaddr + IDR);
	id = ioread16(intc_info->pcie_baseaddr + VER);

	dev_info(&dev->dev, "id = %x, Version = %x\n", id, version);

	// All interrupts map to vector message 0.
	iowrite32(0, intc_info->pcie_baseaddr + IVM + 0x0);
	iowrite32(0, intc_info->pcie_baseaddr + IVM + 0x4);
	iowrite32(0, intc_info->pcie_baseaddr + IVM + 0x8);
	iowrite32(0, intc_info->pcie_baseaddr + IVM + 0xc);

	iowrite16(0, intc_info->pcie_baseaddr + IER);

	vec = pci_alloc_irq_vectors(dev, 1, 1, PCI_IRQ_MSI);
	if (vec < 0) {
		dev_err(&dev->dev, "Unable to enable MSI\n");
		// TODO: handle
	}

	dev_info(&dev->dev, "Got %d vectors for irq use\n", vec);

	dev_info(&dev->dev, "dev->irq = %u\n", dev->irq);
	dev_info(&dev->dev, "IRQ = %u\n", pci_irq_vector(dev, 0));

	intc_info->domain =
		irq_domain_add_linear(fpga_node, 32, &imsar_pcie_intc_chip_ops, intc_info);
	if (!intc_info->domain) {
		dev_err(&dev->dev, "Unable to create IRQ domain\n");
		// TODO: handle
	}

	dev_info(&dev->dev, "IRQ Domain = %p\n", intc_info->domain);

	irq_set_handler_data(dev->irq, intc_info);
	irq_set_handler(dev->irq, imsar_pcie_int_irq_flow_handler);

	enable_irq(dev->irq);

	// Enable PCIe interrupt 0 from AXI intc
	iowrite16(0x1, intc_info->pcie_baseaddr + SIE);

	/*
	 * Disable all external interrupts until they are
	 * explicity requested.
	 */
	iowrite32(0, intc_info->x_baseaddr + X_IER);

	/* Acknowledge any pending interrupts just in case. */
	iowrite32(0xffffffff, intc_info->x_baseaddr + X_IAR);
	// Master enable on the AXI intc
	iowrite32(X_MER_HIE | X_MER_ME, intc_info->x_baseaddr + X_MER);

	return rv;
}

void imsar_pcie_cleanup_interrupts(struct pci_dev *dev)
{
	struct imsar_pcie_intc_info *intc_info;
	struct imsar_pcie *drv_data = (struct imsar_pcie *)pci_get_drvdata(dev);
	intc_info = drv_data->intc_info;

	if (intc_info) {
		// Disable PCIe interrupt 0 from AXI intc
		iowrite16(0x4, intc_info->pcie_baseaddr + CIE);

		disable_irq(dev->irq);

		if (intc_info->domain) {
			irq_domain_remove(intc_info->domain);
		}

		pci_iounmap(dev, intc_info->pcie_baseaddr);
		kfree(intc_info);
	}

	if (dev->msi_enabled) {
		pci_free_irq_vectors(dev);
	}

	pci_release_region(dev, INT_BAR);
}
