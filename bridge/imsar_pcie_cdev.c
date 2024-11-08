#include "imsar_pcie_bridge.h"

#include <asm/uaccess.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/uio.h>
#include <linux/slab.h>
#include <linux/platform_device.h>

#define CDEV_NAME "imsar_nail"
#define MAGIC_CHAR 0xAACC5533UL

struct imsar_pcie_cdev_info {
	const char *name;
	dev_t dev_num;
	phys_addr_t addr;
	resource_size_t size;
	int pos;
	void __iomem *vaddr;
};

struct imsar_pcie_nail_info {
	struct cdev cdev;
	struct imsar_pcie_cdev_info *info;
	int cdev_count;
	struct class *cls;
	unsigned long magic;
	// int major;
};

static int imsar_pcie_cdev_open(struct inode *inode, struct file *file)
{
	struct imsar_pcie_nail_info *nail;
	int index = iminor(inode);

	/* pointer to containing structure of the character device inode */
	nail = container_of(inode->i_cdev, struct imsar_pcie_nail_info, cdev);
	if (nail->magic != MAGIC_CHAR) {
		pr_err("xcdev 0x%p inode 0x%lx magic mismatch 0x%lx\n", nail, inode->i_ino,
		       nail->magic);
		return -EINVAL;
	}
	/* create a reference to our char device in the opened file */
	file->private_data = &nail->info[index];

	return 0;
}

static ssize_t imsar_pcie_cdev_check_transfer(struct file *file, const char __user *buf,
					      size_t count, loff_t *pos)
{
	struct imsar_pcie_cdev_info *info = file->private_data;

	if (count & 3) {
		pr_err("Buffer size must be a multiple of 4 bytes. Not %ld\n", count);
		return -EINVAL;
	}

	if (!buf) {
		pr_err("Caught NULL pointer\n");
		return -EINVAL;
	}

	if (info->dev_num != file->f_inode->i_rdev) {
		pr_err("Bad mapping\n");
		return -EINVAL;
	}
	return 0;
}

static ssize_t imsar_pcie_cdev_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
	struct imsar_pcie_cdev_info *info = file->private_data;
	int missing;
	ssize_t rv;
	ssize_t copied;
	size_t remaining = info->size - *pos;

	if (remaining == 0)
		return 0; //EOF

	rv = imsar_pcie_cdev_check_transfer(file, buf, count, pos);
	if (rv)
		return rv;

	count = min(count, remaining);
	missing = copy_to_user(buf, info->vaddr + *pos, count);

	if (missing == count)
		return -EFAULT;

	copied = count - missing;
	*pos += copied;
	return copied;
}

static ssize_t imsar_pcie_cdev_write(struct file *file, const char __user *buf, size_t count,
				     loff_t *pos)
{
	struct imsar_pcie_cdev_info *info = file->private_data;
	int missing;
	ssize_t rv;
	ssize_t copied;
	size_t remaining = info->size - *pos;

	if (remaining == 0)
		return -EFBIG;

	rv = imsar_pcie_cdev_check_transfer(file, buf, count, pos);
	if (rv)
		return rv;

	count = min(count, remaining);
	missing = copy_from_user(info->vaddr + *pos, buf, count);

	if (missing == count)
		return -EFAULT;

	copied = count - missing;
	*pos += copied;
	return copied;
}

static loff_t imsar_pcie_cdev_llseek(struct file *file, loff_t off, int whence)
{
	loff_t newpos = 0;
	struct imsar_pcie_cdev_info *info = file->private_data;

	switch (whence) {
	case SEEK_SET:
		newpos = off;
		break;
	case SEEK_CUR:
		newpos = file->f_pos + off;
		break;
	case SEEK_END:
		return -EINVAL;
		break;
	default: /* can't happen */
		return -EINVAL;
	}
	if (newpos < 0 || newpos >= info->size)
		return -EINVAL;

	file->f_pos = newpos;
	pr_debug("%s: pos=%lld\n", __func__, (signed long long)newpos);

#ifdef DEBUG
	pr_err("0x%p, off %lld, whence %d -> pos %lld.\n", file, (signed long long)off, whence,
	       (signed long long)off);
#endif

	return newpos;
}

static int imsar_pcie_cdev_char_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct imsar_pcie_cdev_info *info = file->private_data;

	unsigned long off;
	unsigned long phys;
	unsigned long vsize;
	unsigned long psize;
	int rv;

	off = vma->vm_pgoff << PAGE_SHIFT;
	/* BAR physical address */
	phys = info->addr;
	psize = info->size;

	vsize = vma->vm_end - vma->vm_start;
	/* complete resource */

	if (vsize > psize)
		return -EINVAL;
	/*
         * pages must not be cached as this would result in cache line sized
         * accesses to the end point
         */
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	/*
         * prevent touching the pages (byte access) for swap-in,
         * and prevent the pages from being swapped out
         */
	vma->vm_flags |= (VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
	/* make MMIO accessible to user space */
	rv = io_remap_pfn_range(vma, vma->vm_start, phys >> PAGE_SHIFT, vsize, vma->vm_page_prot);
	pr_debug("vma=0x%p, vma->vm_start=0x%lx, phys=0x%lx, size=%lu = %d\n", vma, vma->vm_start,
		 phys >> PAGE_SHIFT, vsize, rv);

	if (rv)
		return -EAGAIN;
	return 0;
}

static struct file_operations imsar_pcie_cdev_fops = {
	.owner = THIS_MODULE,
	.open = imsar_pcie_cdev_open,
	.read = imsar_pcie_cdev_read,
	.write = imsar_pcie_cdev_write,
	.llseek = imsar_pcie_cdev_llseek,
	.mmap = imsar_pcie_cdev_char_mmap,
};

static ssize_t name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct imsar_pcie_cdev_info *cdev = dev_get_drvdata(dev);
	return sprintf(buf, "%s\n", cdev->name);
}
static DEVICE_ATTR_RO(name);

static ssize_t addr_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct imsar_pcie_cdev_info *cdev = dev_get_drvdata(dev);
	return sprintf(buf, "0x%llx\n", cdev->addr);
}
static DEVICE_ATTR_RO(addr);

static ssize_t size_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct imsar_pcie_cdev_info *cdev = dev_get_drvdata(dev);
	return sprintf(buf, "%lld\n", cdev->size);
}
static DEVICE_ATTR_RO(size);

static struct attribute *imsar_pcie_cdev_nail_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_addr.attr,
	&dev_attr_size.attr,
	NULL,
};
ATTRIBUTE_GROUPS(imsar_pcie_cdev_nail);

static int imsar_pcie_create_cdev(struct pci_dev *pci_dev, struct device_node *nail_node)
{
	const int MINOR_BASE = 0;
	int rv = -EIO;
	dev_t dev_num;
	// struct class *cls;
	struct imsar_pcie_nail_info *nail;
	struct device_node *child;
	u32 index = 0;
	u32 ii = 0;
	int child_count = of_get_child_count(nail_node);
	struct imsar_pcie *drvdata = (struct imsar_pcie *)pci_get_drvdata(pci_dev);
	u64 base_addr = pci_resource_start(pci_dev, NAIL_BAR);

	nail = devm_kzalloc(&pci_dev->dev, sizeof(struct imsar_pcie_nail_info), GFP_KERNEL);
	if (!nail) {
		return -ENOMEM;
	}

	nail->info = devm_kzalloc(
		&pci_dev->dev, (child_count + 1) * sizeof(struct imsar_pcie_cdev_info), GFP_KERNEL);
	if (!nail->info) {
		return -ENOMEM;
	}

	drvdata->nail = nail;

	nail->magic = MAGIC_CHAR;
	nail->cdev_count = child_count;

	rv = alloc_chrdev_region(&dev_num, MINOR_BASE, child_count, CDEV_NAME);
	if (rv) {
		dev_err(&pci_dev->dev, "Unable to allocate cdev region %d.\n", rv);
		goto cleanup_mem;
	}

	nail->cls = class_create(THIS_MODULE, CDEV_NAME);
	if (!nail->cls) {
		dev_err(&pci_dev->dev, "Unable to create cdev class\n");
		rv = -ENOMEM;
		goto cleanup_chrdev;
	}
	nail->cls->dev_groups = imsar_pcie_cdev_nail_groups;

	for_each_child_of_node (nail_node, child) {
		u32 reg_prop[2];
		struct imsar_pcie_cdev_info *cdev = &nail->info[index];
		struct device *device;

		dev_t child_dev = MKDEV(MAJOR(dev_num), MINOR(dev_num) + index);
		device = device_create(nail->cls, NULL, child_dev, nail, "nail%d", index);
		if (IS_ERR_OR_NULL(device)) {
			dev_err(&pci_dev->dev, "Unable to create device\n");
			rv = -EIO;
			goto cleanup_class;
		}

		if (of_property_read_u32_array(child, "reg", reg_prop, 2)) {
			dev_err(&pci_dev->dev, "Unable to get reg for %s\n", child->name);
		} else {
			cdev->addr = reg_prop[0] + base_addr;
			cdev->size = reg_prop[1];
			cdev->vaddr = devm_ioremap(&pci_dev->dev, cdev->addr, cdev->size);
		}

		dev_set_drvdata(device, cdev);
		cdev->name = child->name;
		cdev->dev_num = child_dev;

		dev_info(&pci_dev->dev, "Address of %s is %llx, size = %lld\n", cdev->name,
			 cdev->addr, cdev->size);

		index++;
	}

	cdev_init(&nail->cdev, &imsar_pcie_cdev_fops);
	if (cdev_add(&nail->cdev, dev_num, nail->cdev_count)) {
		goto cleanup;
	}

	return 0;

cleanup:
cleanup_class:
	for (ii = 0; ii < index; ii++) {
		device_destroy(nail->cls, nail->info[ii].dev_num);
	}

	class_destroy(nail->cls);
cleanup_chrdev:
	unregister_chrdev_region(dev_num, nail->cdev_count);
cleanup_mem:
	drvdata->nail = NULL;

	return rv;
}

static void imsar_pcie_destroy_cdev(struct pci_dev *pci_dev)
{
	struct imsar_pcie_nail_info *nail;
	struct imsar_pcie *drvdata = (struct imsar_pcie *)pci_get_drvdata(pci_dev);
	int ii;

	nail = drvdata->nail;
	if (!nail) {
		return;
	}

	cdev_del(&nail->cdev);

	for (ii = 0; ii < nail->cdev_count; ii++) {
		device_destroy(nail->cls, nail->info[ii].dev_num);
	}

	class_destroy(nail->cls);

	unregister_chrdev_region(nail->info[0].dev_num, nail->cdev_count);

	drvdata->nail = NULL;
}

int imsar_pcie_setup_nail(struct pci_dev *dev, struct device_node *fpga_node)
{
	struct device_node *nail_node;
	int rv = 0;

	if (pci_request_region(dev, NAIL_BAR, "bar3_msi_int")) {
		dev_err(&dev->dev, "pci_request_region\n");
		return -ENOMEM;
	}

	nail_node = of_get_child_by_name(fpga_node, "nail");
	if (!nail_node) {
		pr_err("Didn't find nail device.  No registers will be enabled\n");
		return -1;
	}

	rv = imsar_pcie_create_cdev(dev, nail_node);
	if (rv)
		return rv;

	return 0;
}

void imsar_pcie_cleanup_nail(struct pci_dev *dev)
{
	imsar_pcie_destroy_cdev(dev);
	pci_release_region(dev, NAIL_BAR);
}
