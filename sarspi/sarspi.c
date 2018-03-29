/*
 * Simple synchronous userspace interface to SPI devices
 *
 * Copyright (C) 2006 SWAPP
 *	Andrea Paterniani <a.paterniani@swapp-eng.it>
 * Copyright (C) 2007 David Brownell (simplification, cleanup)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <asm-generic/errno.h>
#include <asm-generic/ioctl.h>

#include <linux/init.h>
#include <linux/module.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/compat.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
//#include <linux/drivers/base/regmap/internal.h>

#include <linux/spi/spi.h>
#include <linux/spi/spidev.h>

#include <linux/uaccess.h>

#include "version.h"

/*
 * This supports access to SPI devices using normal userspace I/O calls.
 * Note that while traditional UNIX/POSIX I/O semantics are half duplex,
 * and often mask message boundaries, full SPI support requires full duplex
 * transfers.  There are several kinds of internal message boundaries to
 * handle chipselect management and other protocol options.
 *
 * SPI has a character major number assigned.  We allocate minor numbers
 * dynamically using a bitmask.  You must use hotplug tools, such as udev
 * (or mdev with busybox) to create and destroy the /dev/spidevB.C device
 * nodes, since there is no fixed association of minor numbers with any
 * particular SPI bus or device.
 */
static int major = 0;
#define N_SPI_MINORS			32	/* ... up to 256 */
#define DRIVER_NAME	"sarspi"
static DECLARE_BITMAP(minors, N_SPI_MINORS);


/* Bit masks for spi_device.mode management.  Note that incorrect
 * settings for some settings can cause *lots* of trouble for other
 * devices on a shared bus:
 *
 *  - CS_HIGH ... this device will be active when it shouldn't be
 *  - 3WIRE ... when active, it won't behave as it should
 *  - NO_CS ... there will be no explicit message boundaries; this
 *	is completely incompatible with the shared bus model
 *  - READY ... transfers may proceed when they shouldn't.
 *
 * REVISIT should changing those flags be privileged?
 */
#define SPI_MODE_MASK		(SPI_CPHA | SPI_CPOL | SPI_CS_HIGH \
				| SPI_LSB_FIRST | SPI_3WIRE | SPI_LOOP \
				| SPI_NO_CS | SPI_READY | SPI_TX_DUAL \
				| SPI_TX_QUAD | SPI_RX_DUAL | SPI_RX_QUAD)

struct sarspi_data {
	const char		*name;
	dev_t			devt;
	spinlock_t		spi_lock;
	struct spi_device	*spi;
	struct list_head	device_entry;

	/* TX/RX buffers are NULL unless this device is open (users > 0) */
	struct mutex		buf_lock;
	unsigned		users;
	u8			*tx_buffer;
	u8			*rx_buffer;
	u32			speed_hz;

	struct device		*dev;
	struct regmap		*regmap;
	struct regmap_config	*regcfg;
	int			reg_attrs;
	struct device_attribute	*attr_array;
	struct attribute	**attr_list;
	struct attribute_group	*reg_attr_group;

};

static LIST_HEAD(device_list);
static DEFINE_MUTEX(device_list_lock);

static unsigned bufsiz = 4096;
module_param(bufsiz, uint, S_IRUGO);
MODULE_PARM_DESC(bufsiz, "data bytes in biggest supported SPI message");

/*-------------------------------------------------------------------------*/

static ssize_t
spidev_sync(struct sarspi_data *spidev, struct spi_message *message)
{
	DECLARE_COMPLETION_ONSTACK(done);
	int status;
	struct spi_device *spi;

	spin_lock_irq(&spidev->spi_lock);
	spi = spidev->spi;
	spin_unlock_irq(&spidev->spi_lock);

	if (spi == NULL)
		status = -ESHUTDOWN;
	else
		status = spi_sync(spi, message);

	if (status == 0)
		status = message->actual_length;

	return status;
}

static inline ssize_t
spidev_sync_write(struct sarspi_data *spidev, size_t len)
{
	struct spi_transfer	t = {
			.tx_buf		= spidev->tx_buffer,
			.len		= len,
			.speed_hz	= spidev->speed_hz,
		};
	struct spi_message	m;

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	return spidev_sync(spidev, &m);
}

static inline ssize_t
spidev_sync_read(struct sarspi_data *spidev, size_t len)
{
	struct spi_transfer	t = {
			.rx_buf		= spidev->rx_buffer,
			.tx_buf		= spidev->tx_buffer,
			.len		= len,
			.speed_hz	= spidev->speed_hz,
		};
	struct spi_message	m;

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	return spidev_sync(spidev, &m);
}

/*-------------------------------------------------------------------------*/

/* Read-only message with current device setup */
static ssize_t
spidev_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	struct sarspi_data	*spidev;
	ssize_t			status = 0;

	/* chipselect only toggles at start or end of operation */
	if (count > bufsiz)
		return -EMSGSIZE;

	spidev = filp->private_data;

	mutex_lock(&spidev->buf_lock);

	// Transmit the user supplied buffer for full duplex..
	status = copy_from_user(spidev->tx_buffer, buf, count);

	status = spidev_sync_read(spidev, count);
	if (status > 0) {
		unsigned long	missing;

		missing = copy_to_user(buf, spidev->rx_buffer, status);
		if (missing == status)
			status = -EFAULT;
		else
			status = status - missing;
	}
	mutex_unlock(&spidev->buf_lock);

	return status;
}

/* Write-only message with current device setup */
static ssize_t
spidev_write(struct file *filp, const char __user *buf,
		size_t count, loff_t *f_pos)
{
	struct sarspi_data	*spidev;
	ssize_t			status = 0;
	unsigned long		missing;

	/* chipselect only toggles at start or end of operation */
	if (count > bufsiz)
		return -EMSGSIZE;

	spidev = filp->private_data;

	mutex_lock(&spidev->buf_lock);
	missing = copy_from_user(spidev->tx_buffer, buf, count);
	if (missing == 0)
		status = spidev_sync_write(spidev, count);
	else
		status = -EFAULT;
	mutex_unlock(&spidev->buf_lock);

	return status;
}

int alloc_buffers(struct sarspi_data* spidev)
{
	if(!spidev->tx_buffer) {
		spidev->tx_buffer = kmalloc(bufsiz, GFP_KERNEL);
		if(!spidev->tx_buffer) {
			dev_dbg(&spidev->spi->dev, "open/ENOMEM\n");
			return -ENOMEM;
		}
	}
	if(!spidev->rx_buffer) {
		spidev->rx_buffer = kmalloc(bufsiz, GFP_KERNEL);
		if(!spidev->rx_buffer) {
			dev_dbg(&spidev->spi->dev, "open/ENOMEM\n");
			kfree(spidev->tx_buffer);
			spidev->tx_buffer = NULL;
			return -ENOMEM;
		}
	}
	return 0;
}

void dealloc_buffers(struct sarspi_data* spidev)
{
	kfree(spidev->tx_buffer);
	spidev->tx_buffer = NULL;

	kfree(spidev->rx_buffer);
	spidev->rx_buffer = NULL;

}

static int spidev_open(struct inode *inode, struct file *filp)
{
	struct sarspi_data	*spidev;
	int			status = -ENXIO;

	mutex_lock(&device_list_lock);

	list_for_each_entry(spidev, &device_list, device_entry) {
		if (spidev->devt == inode->i_rdev) {
			status = 0;
			break;
		}
	}

	if (status) {
		pr_debug(DRIVER_NAME ": nothing for minor %d\n", iminor(inode));
		goto err_find_dev;
	}

//	status = alloc_buffers(spidev);
	if (status) {
		pr_debug(DRIVER_NAME ": Failed to allocate buffers.\n");
		goto err_find_dev;
	}

	spidev->users++;
	filp->private_data = spidev;
	nonseekable_open(inode, filp);

	mutex_unlock(&device_list_lock);
	return 0;

err_find_dev:
	mutex_unlock(&device_list_lock);
	return status;
}

static int spidev_release(struct inode *inode, struct file *filp)
{
	struct sarspi_data	*spidev;

	mutex_lock(&device_list_lock);
	spidev = filp->private_data;
	filp->private_data = NULL;

	/* last close? */
	spidev->users--;
	if (!spidev->users) {
		int		dofree;

		spin_lock_irq(&spidev->spi_lock);
		if (spidev->spi)
			spidev->speed_hz = spidev->spi->max_speed_hz;

		/* ... after we unbound from the underlying device? */
		dofree = (spidev->spi == NULL);
		spin_unlock_irq(&spidev->spi_lock);

		if (dofree)
			kfree(spidev);
	}
	mutex_unlock(&device_list_lock);

	return 0;
}

static const struct file_operations spidev_fops = {
	.owner =	THIS_MODULE,
	/* REVISIT switch to aio primitives, so that userspace
	 * gets more complete API coverage.  It'll simplify things
	 * too, except for the locking.
	 */
	.write =	spidev_write,
	.read =		spidev_read,
	.open =		spidev_open,
	.release =	spidev_release,
	.llseek =	no_llseek,
};


int hmcmode_write(void *context, unsigned int reg, unsigned int val)
{
	struct sarspi_data* spidev = context;
	int status = 0;
	u32 tx_value = cpu_to_be32( ((val & 0xffffff) << 8) | (reg<<3));

	struct spi_transfer	t = {
			.tx_buf		= &tx_value,
			.len		= 4,
			.speed_hz	= spidev->speed_hz,
		};
	struct spi_message	m;
	u8 *tx_buf = &tx_value;

#ifdef DEBUG
	printk(KERN_DEBUG "Writing %02x %02x %02x %02x \n", tx_buf[0], tx_buf[1], tx_buf[2], tx_buf[3]);
	printk(KERN_DEBUG "Writing 0x%08x\n", tx_value);
#endif
	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	status = spidev_sync(spidev, &m);

	return 0;
}


int hmcmode_read(void *context, unsigned int reg, unsigned int *ret)
{
	u32 rx_stream;
	u32 val;
	struct sarspi_data* spidev = context;
	int status = 0;
	struct spi_transfer	t = {
			.rx_buf		= &rx_stream,
			.len		= 4,
			.speed_hz	= spidev->speed_hz,
		};
	struct spi_message	m;

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);

	//Put Read Address at reg0
	hmcmode_write(context, 0, reg);

	//Do the read
	status = spidev_sync(spidev, &m);
	if (status < 0)
	{
		printk(KERN_ERR "Unable to send data to HMC spi\n");
		return status;
	}
	val = be32_to_cpu(rx_stream);
	val = (val >> 7) & 0xFFFFFF;
	*ret = val;
	return 0;
}


static ssize_t spidev_reg_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned int addr;
	unsigned int val = 0;
	struct spi_device *spi = to_spi_device(dev);
	struct sarspi_data *spidev = spi_get_drvdata(spi);

	if(kstrtouint(attr->attr.name, 16, &addr))
	{
		printk(KERN_ERR "Unable to parse Address.  Bad attribute name: 0x%s\n", attr->attr.name);
		return 0;
	}
	regmap_read(spidev->regmap, addr, &val);
	return snprintf(buf, PAGE_SIZE, "0x%x\n", val);
}

static ssize_t spidev_reg_store(struct device *dev, struct device_attribute *attr,
		 const char *buf, size_t count)
{
	unsigned int addr;
	unsigned int val;
	struct spi_device *spi = to_spi_device(dev);
	struct sarspi_data *spidev = spi_get_drvdata(spi);

	if(kstrtouint(attr->attr.name, 16, &addr))
	{
		printk(KERN_ERR "Unable to parse Address.  Bad attribute name: 0x%s\n", attr->attr.name);
		return count;
	}

	if(kstrtouint(buf, 0, &val))
	{
		printk(KERN_ERR "Unable to parse int from value: %s\n", buf);
		return count;
	}

	regmap_write(spidev->regmap, addr, val);

	return count;
}

static ssize_t spidev_name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct spi_device *spi = to_spi_device(dev);
	struct sarspi_data *spidev = spi_get_drvdata(spi);
	if(dev)
		return snprintf(buf, PAGE_SIZE, "%s\n", spidev->name);
	else
		return 0;
}
static DEVICE_ATTR(name, S_IRUGO, spidev_name_show, NULL);

static struct attribute *attrs[] = {
	&dev_attr_name.attr,
	NULL,
};

static struct attribute_group attr_group = {
		.attrs = attrs,
};

static const struct attribute_group *attr_groups[] = {
		&attr_group, NULL,
};

static bool regmap_false(struct device *dev, unsigned int reg)
{
	return false;
}

static bool regmap_true(struct device *dev, unsigned int reg)
{
	return true;
}

static const struct regmap_range hmcad1520_range[] =
{
	{.range_min = 0x00, .range_max = 0x00,},
	{.range_min = 0x0F, .range_max = 0x0F },
	{.range_min = 0x11, .range_max = 0x12 },
	{.range_min = 0x24, .range_max = 0x27 },
	{.range_min = 0x2A, .range_max = 0x2B },
	{.range_min = 0x30, .range_max = 0x31 },
	{.range_min = 0x33, .range_max = 0x37 },
	{.range_min = 0x3A, .range_max = 0x3B },
	{.range_min = 0x42, .range_max = 0x42 },
	{.range_min = 0x45, .range_max = 0x46 },
	{.range_min = 0x50, .range_max = 0x50 },
	{.range_min = 0x52, .range_max = 0x53 },
	{.range_min = 0x55, .range_max = 0x56 },
};
static struct regmap_access_table hmcad150_wr_table =
{
		.yes_ranges =   hmcad1520_range,
		.n_yes_ranges = ARRAY_SIZE(hmcad1520_range),
};
static struct regmap_config hmcad1520_regcfg = {
		.reg_bits = 8,
		.val_bits = 16,
		.max_register = 0x56,
		.cache_type = REGCACHE_NONE,
		.wr_table = &hmcad150_wr_table,
		.readable_reg = regmap_false,
};

static struct regmap_config hmc703_regcfg = {
		.reg_bits = 8,
		.val_bits = 24,
		.max_register = 0x14,
		.cache_type = REGCACHE_NONE,
		.writeable_reg = regmap_true,
		.readable_reg = regmap_true,
		.reg_read = hmcmode_read,
		.reg_write = hmcmode_write,
};

static struct regmap_config ad9914_regcfg = {
		.reg_bits = 8,
		.val_bits = 32,
		.max_register = 0x1B,
		.cache_type = REGCACHE_NONE,
		.writeable_reg = regmap_true,
		.readable_reg = regmap_true,
};

/*-------------------------------------------------------------------------*/

/* The main reason to have this class is to make mdev/udev create the
 * /dev/spidevB.C character device nodes exposing our userspace API.
 * It also simplifies memory management.
 */

static struct class *spidev_class;

static const struct of_device_id spidev_dt_ids[] = {
	{ .compatible = "hmc,hmcad1520",	.data = &hmcad1520_regcfg, },
	{ .compatible = "hmc,hmc703",		.data = &hmc703_regcfg, },
//	{ .compatible = "hmc,hmc960lp4e", .data = &devtype, },
	{ .compatible = "ad,ad9914",		.data = &ad9914_regcfg, },
//	{ .compatible = "ad,ad9959",		.data = &ad9914_regcfg, },
//	{ .compatible = "condor_pll:        , .data = &devtype, },
	{ .compatible = "spidev"},
	{ .compatible = DRIVER_NAME},
	{},
};
MODULE_DEVICE_TABLE(of, spidev_dt_ids);

static char name_map[0x100][0x10];


static bool _regmap_check_range_table(struct regmap_config *map,
				      unsigned int reg,
				      const struct regmap_access_table *table)
{
	/* Check "no ranges" first */
	if (regmap_reg_in_ranges(reg, table->no_ranges, table->n_no_ranges))
		return false;

	/* In case zero "yes ranges" are supplied, any reg is OK */
	if (!table->n_yes_ranges)
		return true;

	return regmap_reg_in_ranges(reg, table->yes_ranges,
				    table->n_yes_ranges);
}

static bool regmap_writeable(struct regmap_config *map, unsigned int reg)
{
	if (map->max_register && reg > map->max_register)
		return false;

	if (map->writeable_reg)
		return map->writeable_reg(NULL, reg);

	if (map->wr_table)
		return _regmap_check_range_table(map, reg, map->wr_table);

	return true;
}

static bool regmap_readable(struct regmap_config *map, unsigned int reg)
{
	if (map->max_register && reg > map->max_register)
		return false;

	if (map->readable_reg)
		return map->readable_reg(NULL, reg);

	if (map->rd_table)
		return _regmap_check_range_table(map, reg, map->rd_table);

	return true;
}

static bool regmap_exists(struct regmap_config *map, unsigned int reg)
{
	return regmap_readable(map, reg) | regmap_writeable(map, reg);
}

static void create_reg_attrs(struct sarspi_data *spidev)
{
	int ii;
	int status;
	size_t attr_size;
	size_t attr_list_size;
	int regs = 0;

	if (!spidev->regcfg)
	{
		printk(KERN_ERR "regcfg is empty.\n");
		return;
	}

	// If we need to use special read/write, then we have to pass in a null bus.
	if (spidev->regcfg->reg_write || spidev->regcfg->reg_write)
		spidev->regmap = devm_regmap_init(&spidev->spi->dev, NULL, spidev, spidev->regcfg);
	else
		spidev->regmap = devm_regmap_init_spi(spidev->spi, spidev->regcfg);

	if(!spidev->regmap)
	{
		printk(KERN_ERR "Unable to init regmap\n");
		return;
	}

	for(ii = 0; ii <= spidev->regcfg->max_register; ii++) {
		if(regmap_exists(spidev->regcfg, ii))
			regs++;
	}
	spidev->reg_attrs = regs;
	printk(KERN_DEBUG "Creating %d attributes for %s\n", regs, spidev->name);

	attr_size = regs * sizeof(struct device_attribute);
	spidev->attr_array = (struct device_attribute*)kzalloc(attr_size, GFP_KERNEL);
	if(PTR_ERR_OR_ZERO(spidev->attr_array))
	{
		goto array_error;
	}

	attr_list_size = (regs + 1) * sizeof(struct attribute*);
	spidev->attr_list = (struct attribute**)kzalloc(attr_size, GFP_KERNEL);
	if(PTR_ERR_OR_ZERO(spidev->attr_list))
	{
		goto list_error;
	}

	spidev->reg_attr_group = (struct attribute_group*) kzalloc(sizeof(struct attribute_group*),GFP_KERNEL);
	if(PTR_ERR_OR_ZERO(spidev->attr_list))
	{
		goto group_error;
	}

	regs = 0;
	for(ii = 0; ii <= spidev->regcfg->max_register; ii++)
	{
		mode_t mode = 0;
		if (regmap_readable(spidev->regcfg, ii))
			mode |= S_IRUGO;
		if (regmap_writeable(spidev->regcfg, ii))
			mode |= S_IWUGO;
		if(mode)
		{
			sysfs_attr_init(attrs[regs]);
			sprintf(name_map[ii], "%02x", ii);
			spidev->attr_array[regs].attr.name = name_map[ii];
			spidev->attr_array[regs].attr.mode = mode;
			spidev->attr_array[regs].show = spidev_reg_show;
			spidev->attr_array[regs].store = spidev_reg_store;
			spidev->attr_list[regs] = &spidev->attr_array[regs].attr;
			regs++;
		}
	}
	spidev->attr_list[regs] = 0;
	spidev->reg_attr_group->attrs = spidev->attr_list;
	spidev->reg_attr_group->name ="regs";
	status = sysfs_create_group(&spidev->spi->dev.kobj, spidev->reg_attr_group);
	if (status)
		printk(KERN_ERR "Failed to create register attributes: %d\n", status);
	return;

group_error:
	spidev->reg_attr_group = 0;
list_error:
	kfree(spidev->attr_list);
	spidev->attr_list = 0;
array_error:
	kfree(spidev->attr_array);
	spidev->attr_array = 0;

	printk(KERN_ERR "Unable to allocate register attributes\n");

}

/*-------------------------------------------------------------------------*/

static int spidev_probe(struct spi_device *spi)
{
	struct sarspi_data	*spidev;
	int			status;
	unsigned long		minor;

	const struct of_device_id *of_id = of_match_device(spidev_dt_ids, &spi->dev);
	if (!of_id)
		return -ENODEV;

	/* Allocate driver data */
	spidev = kzalloc(sizeof(*spidev), GFP_KERNEL);
	if (!spidev)
		return -ENOMEM;

	/* Initialize the driver data */
	spidev->regcfg = (struct regmap_config *)of_id->data;
	spidev->name = spi->dev.of_node->name;
	status = alloc_buffers(spidev);
	if (status) {
		pr_debug(DRIVER_NAME ": Failed to allocate buffers.\n");
		return status;
	}

	spidev->spi = spi;
	spin_lock_init(&spidev->spi_lock);
	mutex_init(&spidev->buf_lock);

	INIT_LIST_HEAD(&spidev->device_entry);

	/* If we can allocate a minor number, hook up this device.
	 * Reusing minors is fine so long as udev or mdev is working.
	 */
	mutex_lock(&device_list_lock);
	minor = find_first_zero_bit(minors, N_SPI_MINORS);
	if (minor < N_SPI_MINORS) {
		spidev->devt = MKDEV(major, minor);
		spidev->dev = device_create(spidev_class, &spi->dev, spidev->devt,
				    spidev, DRIVER_NAME "%d.%d",
				    spi->master->bus_num, spi->chip_select);
		status = PTR_ERR_OR_ZERO(spidev->dev);
		create_reg_attrs(spidev);

	} else {
		dev_dbg(&spi->dev, "no minor number available!\n");
		status = -ENODEV;
	}
	if (status == 0) {
		set_bit(minor, minors);
		list_add(&spidev->device_entry, &device_list);
	}
	mutex_unlock(&device_list_lock);


	spidev->speed_hz = spi->max_speed_hz;

	if (status == 0)
		spi_set_drvdata(spi, spidev);
	else
		kfree(spidev);

	return status;
}

static int spidev_remove(struct spi_device *spi)
{
	struct sarspi_data	*spidev = spi_get_drvdata(spi);

	if (spidev->reg_attr_group)
		sysfs_remove_group(&spidev->spi->dev.kobj, spidev->reg_attr_group);
	if (spidev->reg_attr_group)
		kfree(spidev->reg_attr_group);
	if (spidev->attr_list)
		kfree(spidev->attr_list);
	if (spidev->attr_array)
		kfree(spidev->attr_array);

	dealloc_buffers(spidev);
	/* make sure ops on existing fds can abort cleanly */
	spin_lock_irq(&spidev->spi_lock);
	spidev->spi = NULL;
	spin_unlock_irq(&spidev->spi_lock);

	/* prevent new opens */
	mutex_lock(&device_list_lock);
	list_del(&spidev->device_entry);
	device_destroy(spidev_class, spidev->devt);
	clear_bit(MINOR(spidev->devt), minors);

	if (spidev->users == 0)
		kfree(spidev);
	mutex_unlock(&device_list_lock);

	return 0;
}

static struct spi_driver spidev_spi_driver = {
	.driver = {
		.name =		DRIVER_NAME,
		.of_match_table = of_match_ptr(spidev_dt_ids),
	},
	.probe =	spidev_probe,
	.remove =	spidev_remove,

	/* NOTE:  suspend/resume methods are not necessary here.
	 * We don't do anything except pass the requests to/from
	 * the underlying controller.  The refrigerator handles
	 * most issues; the controller driver handles the rest.
	 */
};

/*-------------------------------------------------------------------------*/

static int __init spidev_init(void)
{
	int status;

	printk(KERN_INFO "%s version: %s (%s)\n", "ImSAR SARSPI spidev driver", GIT_DESCRIBE, BUILD_DATE);

	/* Claim our 256 reserved device numbers.  Then register a class
	 * that will key udev/mdev to add/remove /dev nodes.  Last, register
	 * the driver which manages those device numbers.
	 */
	BUILD_BUG_ON(N_SPI_MINORS > 256);
	major = register_chrdev(major, "spi", &spidev_fops);
	if (major < 0)
	{
		printk(KERN_ERR "failed to register device: error %d\n", major);
		return major;
	}
	spidev_class = class_create(THIS_MODULE, DRIVER_NAME);
	if (IS_ERR(spidev_class)) {
		unregister_chrdev(major, spidev_spi_driver.driver.name);
		return PTR_ERR(spidev_class);
	}
	spidev_class->dev_groups = attr_groups;

	status = spi_register_driver(&spidev_spi_driver);
	if (status < 0) {
		class_destroy(spidev_class);
		unregister_chrdev(major, spidev_spi_driver.driver.name);
	}

	return status;
}
module_init(spidev_init);

static void __exit spidev_exit(void)
{
	spi_unregister_driver(&spidev_spi_driver);
	class_destroy(spidev_class);
	unregister_chrdev(major, spidev_spi_driver.driver.name);
}
module_exit(spidev_exit);

MODULE_AUTHOR("Andrea Paterniani, <a.paterniani@swapp-eng.it>");
MODULE_DESCRIPTION("User mode SPI device interface");
MODULE_LICENSE("GPL");
MODULE_VERSION(GIT_DESCRIBE);
MODULE_ALIAS("spi:" DRIVER_NAME);
