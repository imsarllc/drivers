/*
 * IMSAR Memmory Mapped Device wrapper
 * Based on the Xilinx AXI Performance Monitor
 *
 * Copyright (C) 2017 IMSAR, LLC. All rights reserved.
 *
 * Description:
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/clk.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uio_driver.h>
#include "version.h"

#define DEVICE_NAME       "uio-imsar"
#define MMDEV_IS_OFFSET   0x0038  /* Interrupt Status Register */
#define UIO_DUMMY_MEMSIZE 4096

/**
 * struct mmdev_param - HW parameters structure
 * @isr: Interrupts info shared to userspace
 */
struct mmdev_param {
	u32 isr;
};

/**
 * struct mmdev_dev - Global driver structure
 * @info: uio_info structure
 * @param: mmdev_param structure
 * @regs: IOmapped base address
 */
struct mmdev_dev {
	struct uio_info info;
	struct mmdev_param param;
	void __iomem *regs;
};

/**
 * mmdev_handler - Interrupt handler for APM
 * @irq: IRQ number
 * @info: Pointer to uio_info structure
 */
static irqreturn_t mmdev_handler(int irq, struct uio_info *info)
{
	struct mmdev_dev *mmdev = (struct mmdev_dev *)info->priv;
	void *ptr;

	ptr = (unsigned long *)mmdev->info.mem[1].addr;
	/* Clear the interrupt and copy the ISR value to userspace */
	mmdev->param.isr = readl(mmdev->regs + MMDEV_IS_OFFSET);
	writel(mmdev->param.isr, mmdev->regs + MMDEV_IS_OFFSET);
	memcpy(ptr, &mmdev->param, sizeof(struct mmdev_param));

	return IRQ_HANDLED;
}

/**
 * mmdev_probe - Driver probe function
 * @pdev: Pointer to the platform_device structure
 *
 * Returns '0' on success and failure value on error
 */

static int mmdev_probe(struct platform_device *pdev)
{
	struct mmdev_dev *mmdev;
	struct resource *res;
	int irq;
	int ret;

	dev_info(&pdev->dev, "%s version: %s (%s)\n", "IMSAR uio driver", GIT_DESCRIBE, BUILD_DATE);

	mmdev = devm_kzalloc(&pdev->dev, (sizeof(struct mmdev_dev)), GFP_KERNEL);
	if(!mmdev)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mmdev->regs = devm_ioremap_resource(&pdev->dev, res);
	if(!mmdev->regs) {
		dev_err(&pdev->dev, "unable to iomap registers\n");
		return -ENOMEM;
	}

	mmdev->info.mem[0].name = DEVICE_NAME;
	mmdev->info.mem[0].addr = res->start;
	mmdev->info.mem[0].size = resource_size(res);
	mmdev->info.mem[0].memtype = UIO_MEM_PHYS;

	ret = of_property_read_string(pdev->dev.of_node, "imsar,name", &mmdev->info.name);
	if(ret < 0) {
		dev_info(&pdev->dev, "no property imsar,name, using device name: %s\n", pdev->dev.of_node->name);
		mmdev->info.name = pdev->dev.of_node->name;
	}

	mmdev->info.version = GIT_DESCRIBE;

	irq = platform_get_irq_optional(pdev, 0);
	if(irq >= 0) {
		mmdev->info.irq = irq;
		mmdev->info.handler = mmdev_handler;
		mmdev->info.priv = mmdev;
	}

	ret = uio_register_device(&pdev->dev, &mmdev->info);
	if(ret < 0) {
		dev_err(&pdev->dev, "unable to register to UIO\n");
		return ret;
	}

	platform_set_drvdata(pdev, mmdev);

	dev_info(&pdev->dev, "Probed IMSAR mmdev\n");

	return 0;
}

/**
 * mmdev_remove - Driver remove function
 * @pdev: Pointer to the platform_device structure
 *
 * Always returns '0'
 */
static int mmdev_remove(struct platform_device *pdev)
{
	struct mmdev_dev *mmdev = platform_get_drvdata(pdev);

	uio_unregister_device(&mmdev->info);

	return 0;
}

static struct of_device_id mmdev_of_match[] = {{.compatible = "imsar,mmdev", }, { /* end of table*/}};

MODULE_DEVICE_TABLE(of, mmdev_of_match);

static struct platform_driver mmdev_driver = {
		.driver = {.name = DEVICE_NAME, .of_match_table = mmdev_of_match, },
		.probe = mmdev_probe,
		.remove = mmdev_remove, };

module_platform_driver(mmdev_driver);

MODULE_AUTHOR("IMSAR LLC");
MODULE_DESCRIPTION("IMSAR Memory Mapped Device wrapper");
MODULE_LICENSE("GPL v2");
MODULE_VERSION(GIT_DESCRIBE);
