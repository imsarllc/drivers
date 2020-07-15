/*
 * Xilinx XVC Driver
 * Copyright (C) 2019 Xilinx Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <asm/io.h>
#include <linux/mod_devicetable.h>

#include "xvc_driver.h"
#include "version.h"

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Max Heimer <maxh@xilinx.com>");
MODULE_DESCRIPTION("XVC Debug Register Access");
MODULE_VERSION(GIT_DESCRIBE);

static dev_t xvc_ioc_dev_region;
static struct class* xvc_dev_class = NULL;
static struct cdev xvc_char_ioc_dev;

#ifndef _XVC_USER_CONFIG_H
#define MAX_CONFIG_COUNT 8
#define CONFIG_COUNT 1
#define GET_DB_BY_RES 1
static struct resource *db_res[MAX_CONFIG_COUNT];
#endif /* _XVC_USER_CONFIG_H */

static void __iomem * db_ptrs[MAX_CONFIG_COUNT];

static void xil_xvc_cleanup(void) {
	printk(KERN_INFO LOG_PREFIX "Cleaning up resources...\n");

	if (!IS_ERR(xvc_dev_class)) {
		class_destroy(xvc_dev_class);
		xvc_dev_class = NULL;
		if (xvc_char_ioc_dev.owner != NULL) {
			cdev_del(&xvc_char_ioc_dev);
		}
		unregister_chrdev_region(xvc_ioc_dev_region, MAX_CONFIG_COUNT);
	}
}

long char_ctrl_ioctl(struct file *file_p, unsigned int cmd, unsigned long arg) {
	long status = 0;
	unsigned long irqflags = 0;
	int minor = iminor(file_p->f_path.dentry->d_inode);

	spin_lock_irqsave(&file_p->f_path.dentry->d_inode->i_lock, irqflags);

	switch (cmd) {
		case XDMA_IOCXVC:
			status = xil_xvc_ioctl((unsigned char*)(db_ptrs[minor]), (void __user *)arg);
			break;
		case XDMA_RDXVC_PROPS:
			{
				struct db_config config_info = {
					.name = NULL,
					.base_addr = db_res[minor] ? db_res[minor]->start : 0,
					.size = db_res[minor] ? resource_size(db_res[minor]) : 0,
				};
				status = xil_xvc_readprops(&config_info, (void __user*)arg);
				break;
			}
		default:
			status = -ENOIOCTLCMD;
			break;
	}
#if LINUX_VERSION_CODE <= KERNEL_VERSION(4,6,0)
	mmiowb();
#endif
	spin_unlock_irqrestore(&file_p->f_path.dentry->d_inode->i_lock, irqflags);

	return status;
}

static int xvc_mmap(struct file *filep, struct vm_area_struct *vma)
{
	unsigned long requested_pages, actual_pages;
	unsigned long db_addr = 0;
	unsigned long db_size = 0;
	int minor = iminor(filep->f_path.dentry->d_inode);

	if (vma->vm_end < vma->vm_start)
		return -EINVAL;

	if (db_res[minor]) {
		db_addr = db_res[minor]->start;
		db_size = resource_size(db_res[minor]);
	}

	requested_pages = vma_pages(vma);
	actual_pages = ((db_addr & ~PAGE_MASK)
			+ db_size + PAGE_SIZE -1) >> PAGE_SHIFT;
	if (requested_pages > actual_pages) {
		return -EINVAL;
	}
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	return remap_pfn_range(vma,
			       vma->vm_start,
			       db_addr >> PAGE_SHIFT,
			       vma->vm_end - vma->vm_start,
			       vma->vm_page_prot);
}

static struct file_operations xil_xvc_ioc_ops = {
	.owner = THIS_MODULE,
	.mmap = xvc_mmap,
	.unlocked_ioctl = char_ctrl_ioctl
};

static DEFINE_MUTEX(device_list_lock);
static DECLARE_BITMAP(minors, MAX_CONFIG_COUNT);

int probe(struct platform_device* pdev) {
	int status;
	dev_t ioc_device_number;
	char ioc_device_name[32];
	struct device* xvc_ioc_device = NULL;
	unsigned long minor;
	const char *name;
	unsigned long db_addr = 0;
	unsigned long db_size = 0;
	int ret;

	if (!xvc_dev_class) {
		xvc_dev_class = class_create(THIS_MODULE, XVC_DRIVER_NAME);
		if (IS_ERR(xvc_dev_class)) {
			xil_xvc_cleanup();
			dev_err(&pdev->dev, "unable to create class\n");
			return PTR_ERR(xvc_dev_class);
		}

		cdev_init(&xvc_char_ioc_dev, &xil_xvc_ioc_ops);
		xvc_char_ioc_dev.owner = THIS_MODULE;
		status = cdev_add(&xvc_char_ioc_dev, xvc_ioc_dev_region, MAX_CONFIG_COUNT);
		if (status != 0) {
			xil_xvc_cleanup();
			dev_err(&pdev->dev, "unable to add char device\n");
			return status;
		}
	}


	ret = of_property_read_string(pdev->dev.of_node, "imsar,name", &name);
	if(ret < 0) {
		dev_info(&pdev->dev, "no property imsar,name, using device name: %s\n", pdev->dev.of_node->name);
		name = pdev->dev.of_node->name;
	}
	sprintf(ioc_device_name, "jtag_%s", name);

	mutex_lock(&device_list_lock);
	minor = find_first_zero_bit(minors, MAX_CONFIG_COUNT);
	if (minor < MAX_CONFIG_COUNT) {
		ioc_device_number = MKDEV(MAJOR(xvc_ioc_dev_region), minor);
		xvc_ioc_device = device_create(xvc_dev_class, NULL, ioc_device_number, NULL, ioc_device_name);
		if (PTR_ERR_OR_ZERO(xvc_ioc_device)) {
			dev_warn(&pdev->dev, LOG_PREFIX "Failed to create device %s", ioc_device_name);
			xil_xvc_cleanup();
			dev_err(&pdev->dev, "unable to create the device\n");
			return status;
		} else {
			dev_info(&pdev->dev, LOG_PREFIX "Created device %s", ioc_device_name);
		}
	} else {
		dev_dbg(&pdev->dev, "no minor number available!\n");
		status = -ENODEV;
	}
	if (status == 0) {
		set_bit(minor, minors);
		db_res[minor] = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		db_ptrs[minor] = devm_ioremap_resource(&pdev->dev, db_res[minor]);
	}
	mutex_unlock(&device_list_lock);


	if (db_res[minor]) {
		db_addr = db_res[minor]->start;
		db_size = resource_size(db_res[minor]);
	}
	dev_err(&pdev->dev, LOG_PREFIX "debug bridge %s memory at offset 0x%lX, size %lu", ioc_device_name, db_addr, db_size);
	if (!db_ptrs[minor] || IS_ERR(db_ptrs[minor])) {
		dev_err(&pdev->dev, LOG_PREFIX "Failed to remap debug bridge memory at offset 0x%lX, size %lu", db_addr, db_size);
		return -ENOMEM;
	} else {
		dev_info(&pdev->dev, LOG_PREFIX "Mapped debug bridge at offset 0x%lX, size 0x%lX", db_addr, db_size);
	}

	return 0;
}

static int remove(struct platform_device* pdev) {
	int i;
	dev_t ioc_device_number;
	if (pdev) {
		for (i = 0; i < MAX_CONFIG_COUNT; ++i) {
			if (db_ptrs[i]) {
				unsigned long db_addr = 0;
				unsigned long db_size = 0;
				if (db_res[i]) {
					db_addr = db_res[i]->start;
					db_size = resource_size(db_res[i]);
				}

				dev_info(&pdev->dev, LOG_PREFIX "Unmapping debug bridge at offset 0x%lX, size %lu", db_addr, db_size);
				// devm_ioremap_resource is managed by the kernel and undone on driver detach.
				mutex_lock(&device_list_lock);
				db_ptrs[i] = NULL;
				ioc_device_number = MKDEV(MAJOR(xvc_ioc_dev_region), i);
				device_destroy(xvc_dev_class, ioc_device_number);
				clear_bit(i, minors);
				mutex_unlock(&device_list_lock);
				dev_info(&pdev->dev, LOG_PREFIX "Destroyed device number %u (user config %i)", ioc_device_number, i);
			}
		}
	}
	return 0;
}

static const struct of_device_id xvc_of_ids[] = {
	{ .compatible = DEBUG_BRIDGE_COMPAT_STRING, },
	{}
};

static struct platform_driver xil_xvc_plat_driver = {
	.driver = {
		.name = XVC_DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table = xvc_of_ids,
	},
	.probe = probe,
	.remove = remove,
};

// --------------------------
// --------------------------
// Driver initialization code
// --------------------------
// --------------------------

static int __init xil_xvc_init(void) {
	int err = 0;

	printk(KERN_INFO LOG_PREFIX "%s version: %s (%s)\n", "IMSAR xvc driver", GIT_DESCRIBE, BUILD_DATE);

	// Register the character packet device major and minor numbers
	err = alloc_chrdev_region(&xvc_ioc_dev_region, 0, MAX_CONFIG_COUNT, XVC_DRIVER_NAME);
	if (err != 0) {
		xil_xvc_cleanup();
		printk(KERN_ERR LOG_PREFIX "unable to get char device region\n");
		return err;
	}

	memset(db_ptrs, 0, sizeof(*db_ptrs));

	return platform_driver_register(&xil_xvc_plat_driver);
}

static void __exit xil_xvc_exit(void) {
	platform_driver_unregister(&xil_xvc_plat_driver);
	xil_xvc_cleanup();
}

module_init(xil_xvc_init);
module_exit(xil_xvc_exit);
