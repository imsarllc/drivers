#include <linux/of.h>
#include <linux/pci.h>

#define NAIL_BAR 0
#define AXIL_BAR 1
#define BRIDGE_BAR 2
#define INT_BAR 3

struct nail_info;
struct intc_info;

struct imsar_pcie {
	struct pci_dev *pci;
	struct nail_info *nail;
	struct intc_info *intc_info;
};

int imsar_setup_interrupts(struct pci_dev *dev, struct device_node *fpga_node);
void imsar_cleanup_interrupts(struct pci_dev *dev);

int imsar_setup_nail(struct pci_dev *dev, struct device_node *fpga_node);
void imsar_cleanup_nail(struct pci_dev *dev);