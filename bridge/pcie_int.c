#include "pcie_bridge.h"
#include <linux/module.h>
#include <linux/pci.h>

#include <linux/log2.h>

#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>

// static void __iomem *mmio;
// const int INTERRUPT_MASK_OFFSET = 0x2004;
// const int INTERRUPT_SET_OFFSET = 0x2008;
// const int INTERRUPT_CLEAR_OFFSET = 0x200c;

struct intc_info {
	void __iomem *baseaddr;
	struct irq_domain *domain;
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

static void enable_or_unmask(struct irq_data *data)
{
	unsigned long mask = 1 << data->hwirq;
	struct intc_info *local_intc = irq_data_get_irq_chip_data(data);
	pr_err("PCI: Called enable irq=%d, hw_irq=%lu\n", data->irq, data->hwirq);

	iowrite16(mask, local_intc->baseaddr + SIE);
}

static void disable_or_mask(struct irq_data *data)
{
	unsigned long mask = 1 << data->hwirq;
	struct intc_info *local_intc = irq_data_get_irq_chip_data(data);
	pr_err("PCI: Called disable irq=%d, hw_irq=%lu\n", data->irq, data->hwirq);

	iowrite16(mask, local_intc->baseaddr + CIE);
}

static void ack(struct irq_data *data)
{
	// pr_err("Ack.  Nothing to do.\n");
}

static struct irq_chip intc_dev = {
	.name = "IMSAR MSI interrupt bridge",
	.irq_enable = enable_or_unmask,
	.irq_unmask = enable_or_unmask,
	.irq_disable = disable_or_mask,
	.irq_mask = disable_or_mask,
	.irq_mask_ack = disable_or_mask,
	.irq_ack = ack,
};

static unsigned int get_irq(struct intc_info *local_intc)
{
	int irq = -1;
	unsigned int hwirq;
	unsigned int mask;

	mask = ioread16(local_intc->baseaddr + ISR);
	hwirq = ilog2(mask);
	if (mask)
		irq = irq_find_mapping(local_intc->domain, hwirq);

	pr_debug("get_irq: hwirq=%d, irq=%d\n", hwirq, irq);
	return irq;
}

static int xintc_map(struct irq_domain *d, unsigned int irq, irq_hw_number_t hw)
{
	struct intc_info *local_intc = d->host_data;
	pr_err("Called Interrupt map with domain name %s, irq %d & hw_irq=%ld. Host: %p\n", //
	       d->name, irq, hw, d->host_data);

	irq_set_chip_and_handler_name(irq, &intc_dev, handle_edge_irq, "edge");
	irq_clear_status_flags(irq, IRQ_LEVEL);
	irq_set_chip_data(irq, local_intc);
	return 0;
}

struct irq_domain_ops intc_chip_ops = {
	.xlate = irq_domain_xlate_onecell,
	.map = xintc_map,
};

static void irq_flow_handler(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct intc_info *local_intc = irq_data_get_irq_handler_data(&desc->irq_data);

	int irq;
	pr_info("irq_handler called for %s: %d", desc->name, desc->irq_data.irq);

	chained_irq_enter(chip, desc);

	/*
         * ignore the irq input as we now need to query the AXI interrupt
         * controller to see which interrupt is active
         */
	irq = get_irq(local_intc);
	while (irq != -1) {
		generic_handle_irq(irq);
		irq = get_irq(local_intc);
	}

	chained_irq_exit(chip, desc);
}

int imsar_setup_interrupts(struct pci_dev *dev, struct device_node *fpga_node)
{
	u16 id;
	u16 version;
	int rv = 0;
	u32 vec;
	struct intc_info *intc_info;

	intc_info = kzalloc(sizeof(struct intc_info), GFP_KERNEL);
	if (!intc_info)
		return -ENOMEM;
	((struct imsar_pcie *)pci_get_drvdata(dev))->intc_info = intc_info;

	if (pci_request_region(dev, INT_BAR, "bar3_msi_int")) {
		dev_err(&(dev->dev), "pci_request_region\n");
		return -ENOMEM;
	}
	intc_info->baseaddr = pci_iomap(dev, INT_BAR, pci_resource_len(dev, INT_BAR));
	if (!intc_info->baseaddr)
		return -ENOMEM;

	version = ioread16(intc_info->baseaddr + IDR);
	id = ioread16(intc_info->baseaddr + VER);
	pr_info("id = %x, Version = %x\n", id, version);
	// All interrupts map to vector message 0.
	// Later, we may want to remap internal messages to other iterrupts.
	// iowrite32(0x03020100, intc_info->baseaddr + IVM + 0x0);
	// iowrite32(0x07060504, intc_info->baseaddr + IVM + 0x4);
	// iowrite32(0x0b0a0908, intc_info->baseaddr + IVM + 0x8);
	// iowrite32(0x0f0e0d0c, intc_info->baseaddr + IVM + 0xc);

	iowrite16(0, intc_info->baseaddr + IER);

	vec = pci_alloc_irq_vectors(dev, 1, 16, PCI_IRQ_ALL_TYPES);
	if (vec < 0)
		pr_err("Unable to enable MSI\n");
	pr_info("Got %d vectors for irq use\n", vec);

	pr_info("dev->irq = %u\n", dev->irq);
	pr_info("IRQ = %u\n", pci_irq_vector(dev, 0));

	intc_info->domain = irq_domain_add_linear(fpga_node, 16, &intc_chip_ops, intc_info);
	if (!intc_info->domain)
		pr_err("Unable to create IRQ domain\n");

	pr_info("IRQ Domain = %p\n", intc_info->domain);

	irq_set_handler_data(dev->irq, intc_info);
	irq_set_handler(dev->irq, irq_flow_handler);
	enable_irq(dev->irq);
	// irq_set_handler(dev->irq + 1, irq_flow_handler);
	// irq_set_handler(dev->irq + 2, irq_flow_handler);

	return rv;
}

void imsar_cleanup_interrupts(struct pci_dev *dev)
{
	struct intc_info *intc_info;
	intc_info = ((struct imsar_pcie *)pci_get_drvdata(dev))->intc_info;

	if (intc_info) {
		disable_irq(dev->irq);

		if (intc_info->domain)
			irq_domain_remove(intc_info->domain);
		pci_iounmap(dev, intc_info->baseaddr);
		kfree(intc_info);
	}
	if (dev->msi_enabled)
		pci_free_irq_vectors(dev);

	pci_release_region(dev, INT_BAR);
}
