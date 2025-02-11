/**
 * IMSAR driver for the Xilinx DMA Core in Simple Register Mode
 *
 *
 * Copyright (C) 2025 IMSAR, LLC.
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 *
 */

#include "imsar-xdma-sysfs.h"

#include "imsar-xdma-defs.h"
#include "imsar-xdma-ops.h"

#include <linux/bits.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/wait.h>

///////////////////////////////////////
// Forward function declarations
///////////////////////////////////////

static ssize_t imsar_xdma_sysfs_register_show(const char *fmt, unsigned int reg, struct device *dev, char *buf);
static ssize_t imsar_xdma_sysfs_register_store(unsigned int reg, struct device *dev, const char *buf, size_t size);
static ssize_t imsar_xdma_sysfs_register_query_mask(unsigned int reg, unsigned int mask, struct device *dev, char *buf);
static ssize_t imsar_xdma_sysfs_register_set_mask(unsigned int reg, unsigned int mask, struct device *dev,
                                                  const char *buf, size_t size);

///////////////////////////////////////
// Static variables
///////////////////////////////////////

// ungrouped attributes
static DEVICE_ATTR(name, S_IRUGO, imsar_xdma_sysfs_name_show, NULL);
static struct attribute *imsar_xdma_sysfs_top_attrs[] = {&dev_attr_name.attr, NULL};
static struct attribute_group imsar_xdma_sysfs_top_attr_group = {.attrs = imsar_xdma_sysfs_top_attrs};

// info group
static DEVICE_ATTR(buffer_count, S_IRUGO, imsar_xdma_sysfs_buffer_count_show, NULL);
static DEVICE_ATTR(buffer_size, S_IRUGO, imsar_xdma_sysfs_buffer_size_show, NULL);
static DEVICE_ATTR(transfer_id, S_IRUGO, imsar_xdma_sysfs_transfer_id_show, NULL);
static struct attribute *imsar_xdma_sysfs_info_attrs[] = { //
    &dev_attr_buffer_count.attr,                           //
    &dev_attr_buffer_size.attr,                            //
    &dev_attr_transfer_id.attr,                            //
    NULL};

static struct attribute_group imsar_xdma_sysfs_info_attr_group = {
    .name = "info",
    .attrs = imsar_xdma_sysfs_info_attrs,
};

// log group
static DEVICE_ATTR(log_register_access, (S_IRUGO | S_IWUSR | S_IWGRP), imsar_xdma_sysfs_log_register_access_show,
                   imsar_xdma_sysfs_log_register_access_store);
static DEVICE_ATTR(log_transfer_events, (S_IRUGO | S_IWUSR | S_IWGRP), imsar_xdma_sysfs_log_transfer_events_show,
                   imsar_xdma_sysfs_log_transfer_events_store);
static struct attribute *imsar_xdma_sysfs_log_attrs[] = { //
    &dev_attr_log_register_access.attr,                   //
    &dev_attr_log_transfer_events.attr,                   //
    NULL};
static struct attribute_group imsar_xdma_sysfs_log_attr_group = {
    .name = "log",
    .attrs = imsar_xdma_sysfs_log_attrs,
};

// status group

static DEVICE_ATTR(idle, S_IRUGO, imsar_xdma_sysfs_status_idle_show, NULL);
static DEVICE_ATTR(halted, S_IRUGO, imsar_xdma_sysfs_status_halted_show, NULL);
static DEVICE_ATTR(err_int, S_IRUGO, imsar_xdma_sysfs_status_err_int_show, NULL);
static DEVICE_ATTR(err_slv, S_IRUGO, imsar_xdma_sysfs_status_err_slv_show, NULL);
static DEVICE_ATTR(err_dec, S_IRUGO, imsar_xdma_sysfs_status_err_dec_show, NULL);
static DEVICE_ATTR(irq_ioc, S_IRUGO, imsar_xdma_sysfs_status_irq_ioc_show, NULL);
static DEVICE_ATTR(irq_err, S_IRUGO, imsar_xdma_sysfs_status_irq_err_show, NULL);
static struct attribute *imsar_xdma_sysfs_status_attrs[] = { //
    &dev_attr_idle.attr,                                     //
    &dev_attr_halted.attr,                                   //
    &dev_attr_err_int.attr,                                  //
    &dev_attr_err_slv.attr,                                  //
    &dev_attr_err_dec.attr,                                  //
    &dev_attr_irq_ioc.attr,                                  //
    &dev_attr_irq_err.attr,                                  //
    NULL};
static struct attribute_group imsar_xdma_sysfs_status_attr_group = {
    .name = "status",
    .attrs = imsar_xdma_sysfs_status_attrs,
};

// control group

static DEVICE_ATTR(runstop, (S_IRUGO | S_IWUSR | S_IWGRP), imsar_xdma_sysfs_control_runstop_show,
                   imsar_xdma_sysfs_control_runstop_store);
static DEVICE_ATTR(reset, (S_IRUGO | S_IWUSR | S_IWGRP), imsar_xdma_sysfs_control_reset_show,
                   imsar_xdma_sysfs_control_reset_store);
static DEVICE_ATTR(irq_ioc_en, (S_IRUGO | S_IWUSR | S_IWGRP), imsar_xdma_sysfs_control_irq_ioc_en_show,
                   imsar_xdma_sysfs_control_irq_ioc_en_store);
static DEVICE_ATTR(irq_err_en, (S_IRUGO | S_IWUSR | S_IWGRP), imsar_xdma_sysfs_control_irq_err_en_show,
                   imsar_xdma_sysfs_control_irq_err_en_store);
static struct attribute *imsar_xdma_sysfs_control_attrs[] = { //
    &dev_attr_runstop.attr,                                   //
    &dev_attr_reset.attr,                                     //
    &dev_attr_irq_ioc_en.attr,                                //
    &dev_attr_irq_err_en.attr,                                //
    NULL};
static struct attribute_group imsar_xdma_sysfs_control_attr_group = {
    .name = "control",
    .attrs = imsar_xdma_sysfs_control_attrs,
};

// register group
static DEVICE_ATTR(status, (S_IRUGO | S_IWUSR | S_IWGRP), imsar_xdma_sysfs_reg_status_show,
                   imsar_xdma_sysfs_reg_status_store);
static DEVICE_ATTR(control, (S_IRUGO | S_IWUSR | S_IWGRP), imsar_xdma_sysfs_reg_control_show,
                   imsar_xdma_sysfs_reg_control_store);
static DEVICE_ATTR(address, (S_IRUGO | S_IWUSR | S_IWGRP), imsar_xdma_sysfs_reg_address_show,
                   imsar_xdma_sysfs_reg_address_store);
static DEVICE_ATTR(length, (S_IRUGO | S_IWUSR | S_IWGRP), imsar_xdma_sysfs_reg_length_show,
                   imsar_xdma_sysfs_reg_length_store);
static struct attribute *imsar_xdma_sysfs_register_attrs[] = { //
    &dev_attr_status.attr,                                     //
    &dev_attr_control.attr,                                    //
    &dev_attr_address.attr,                                    //
    &dev_attr_length.attr,                                     //
    NULL};
static struct attribute_group imsar_xdma_sysfs_register_attr_group = {
    .name = "register",
    .attrs = imsar_xdma_sysfs_register_attrs,
};

const struct attribute_group *imsar_xdma_sysfs_attr_groups[] = {
    &imsar_xdma_sysfs_top_attr_group,      //
    &imsar_xdma_sysfs_info_attr_group,     //
    &imsar_xdma_sysfs_log_attr_group,      //
    &imsar_xdma_sysfs_status_attr_group,   //
    &imsar_xdma_sysfs_control_attr_group,  //
    &imsar_xdma_sysfs_register_attr_group, //
    NULL,
};


///////////////////////////////////////
// Function definitions
///////////////////////////////////////

ssize_t imsar_xdma_sysfs_name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	imsar_xdma_channel_t *channel = (imsar_xdma_channel_t *)dev_get_drvdata(dev);
	if (!channel)
	{
		return 0;
	}
	return snprintf(buf, PAGE_SIZE, "%s\n", channel->name);
}

ssize_t imsar_xdma_sysfs_reset_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	imsar_xdma_channel_t *channel = (imsar_xdma_channel_t *)dev_get_drvdata(dev);
	if (!channel)
	{
		return 0;
	}
	imsar_xdma_reset(channel->xdma_device);
	return size;
}

ssize_t imsar_xdma_sysfs_buffer_count_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	imsar_xdma_channel_t *channel = (imsar_xdma_channel_t *)dev_get_drvdata(dev);
	if (!channel)
	{
		return 0;
	}
	return snprintf(buf, PAGE_SIZE, "%u\n", channel->buffer_count);
}

ssize_t imsar_xdma_sysfs_buffer_size_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	imsar_xdma_channel_t *channel = (imsar_xdma_channel_t *)dev_get_drvdata(dev);
	if (!channel)
	{
		return 0;
	}
	return snprintf(buf, PAGE_SIZE, "%u\n", channel->buffer_size_bytes);
}

ssize_t imsar_xdma_sysfs_transfer_id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	imsar_xdma_channel_t *channel = (imsar_xdma_channel_t *)dev_get_drvdata(dev);
	if (!channel)
	{
		return 0;
	}
	return snprintf(buf, PAGE_SIZE, "%u\n", channel->last_finished_transfer_id);
}

ssize_t imsar_xdma_sysfs_register_show(const char *fmt, unsigned int reg, struct device *dev, char *buf)
{
	u32 value;
	imsar_xdma_channel_t *channel = (imsar_xdma_channel_t *)dev_get_drvdata(dev);
	if (!channel)
	{
		return 0;
	}
	value = imsar_xdma_chan_reg_read(channel, reg);
	return snprintf(buf, PAGE_SIZE, fmt, value);
}

ssize_t imsar_xdma_sysfs_register_store(unsigned int reg, struct device *dev, const char *buf, size_t size)
{
	char *end;
	unsigned long value;
	imsar_xdma_channel_t *channel;
	channel = (imsar_xdma_channel_t *)dev_get_drvdata(dev);
	value = simple_strtoul(buf, &end, 0);
	if (end == buf)
	{
		return -EINVAL;
	}
	imsar_xdma_chan_reg_write(channel, reg, value);
	return size;
}

ssize_t imsar_xdma_sysfs_log_register_access_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	imsar_xdma_channel_t *channel = (imsar_xdma_channel_t *)dev_get_drvdata(dev);
	if (!channel)
	{
		return 0;
	}
	return snprintf(buf, PAGE_SIZE, "%u\n", channel->xdma_device->log_register_access);
}

ssize_t imsar_xdma_sysfs_log_register_access_store(struct device *dev, struct device_attribute *attr, const char *buf,
                                                   size_t size)
{
	char *end;
	unsigned long value;
	imsar_xdma_channel_t *channel;
	channel = (imsar_xdma_channel_t *)dev_get_drvdata(dev);
	value = simple_strtoul(buf, &end, 0);
	if (end == buf || (value != 0 && value != 1))
	{
		return -EINVAL;
	}
	channel->xdma_device->log_register_access = value;
	return size;
}

ssize_t imsar_xdma_sysfs_log_transfer_events_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	imsar_xdma_channel_t *channel = (imsar_xdma_channel_t *)dev_get_drvdata(dev);
	if (!channel)
	{
		return 0;
	}
	return snprintf(buf, PAGE_SIZE, "%u\n", channel->log_transfer_events);
}

ssize_t imsar_xdma_sysfs_log_transfer_events_store(struct device *dev, struct device_attribute *attr, const char *buf,
                                                   size_t size)
{
	char *end;
	unsigned long value;
	imsar_xdma_channel_t *channel;
	channel = (imsar_xdma_channel_t *)dev_get_drvdata(dev);
	value = simple_strtoul(buf, &end, 0);
	if (end == buf || (value != 0 && value != 1))
	{
		return -EINVAL;
	}
	channel->log_transfer_events = value;
	return size;
}

static ssize_t imsar_xdma_sysfs_register_query_mask(unsigned int reg, unsigned int mask, struct device *dev, char *buf)
{
	u32 value;
	imsar_xdma_channel_t *channel = (imsar_xdma_channel_t *)dev_get_drvdata(dev);
	if (!channel)
	{
		return 0;
	}
	value = imsar_xdma_chan_reg_read(channel, reg);
	return snprintf(buf, PAGE_SIZE, "%u", value & mask ? 1 : 0);
}

static ssize_t imsar_xdma_sysfs_register_set_mask(unsigned int reg, unsigned int mask, struct device *dev,
                                                  const char *buf, size_t size)
{
	u32 value;
	char *end;
	unsigned long set;
	imsar_xdma_channel_t *channel = (imsar_xdma_channel_t *)dev_get_drvdata(dev);
	if (!channel)
	{
		return 0;
	}

	set = simple_strtoul(buf, &end, 0);
	if (end == buf || (set != 0 && set != 1))
	{
		return -EINVAL;
	}

	value = imsar_xdma_chan_reg_read(channel, reg);
	if (set)
	{
		value |= mask;
	}
	else
	{
		value &= ~mask;
	}

	imsar_xdma_chan_reg_write(channel, reg, value);
	return size;
}

ssize_t imsar_xdma_sysfs_status_idle_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return imsar_xdma_sysfs_register_query_mask(REG_STATUS, FLAG_STATUS_IDLE, dev, buf);
}

ssize_t imsar_xdma_sysfs_status_halted_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return imsar_xdma_sysfs_register_query_mask(REG_STATUS, FLAG_STATUS_HALTED, dev, buf);
}

ssize_t imsar_xdma_sysfs_status_err_int_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return imsar_xdma_sysfs_register_query_mask(REG_STATUS, FLAG_STATUS_DMA_INT_ERR, dev, buf);
}

ssize_t imsar_xdma_sysfs_status_err_slv_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return imsar_xdma_sysfs_register_query_mask(REG_STATUS, FLAG_STATUS_DMA_SLV_ERR, dev, buf);
}

ssize_t imsar_xdma_sysfs_status_err_dec_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return imsar_xdma_sysfs_register_query_mask(REG_STATUS, FLAG_STATUS_DMA_DEC_ERR, dev, buf);
}

ssize_t imsar_xdma_sysfs_status_irq_ioc_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return imsar_xdma_sysfs_register_query_mask(REG_STATUS, FLAG_STATUS_IOC_IRQ, dev, buf);
}

ssize_t imsar_xdma_sysfs_status_irq_err_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return imsar_xdma_sysfs_register_query_mask(REG_STATUS, FLAG_STATUS_ERR_IRQ, dev, buf);
}


ssize_t imsar_xdma_sysfs_control_reset_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return imsar_xdma_sysfs_register_query_mask(REG_CONTROL, FLAG_CONTROL_RESET, dev, buf);
}

ssize_t imsar_xdma_sysfs_control_reset_store(struct device *dev, struct device_attribute *attr, const char *buf,
                                             size_t size)
{
	return imsar_xdma_sysfs_register_set_mask(REG_CONTROL, FLAG_CONTROL_RESET, dev, buf, size);
}

ssize_t imsar_xdma_sysfs_control_runstop_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return imsar_xdma_sysfs_register_query_mask(REG_CONTROL, FLAG_CONTROL_RUNSTOP, dev, buf);
}

ssize_t imsar_xdma_sysfs_control_runstop_store(struct device *dev, struct device_attribute *attr, const char *buf,
                                               size_t size)
{
	return imsar_xdma_sysfs_register_set_mask(REG_CONTROL, FLAG_CONTROL_RUNSTOP, dev, buf, size);
}

ssize_t imsar_xdma_sysfs_control_irq_ioc_en_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return imsar_xdma_sysfs_register_query_mask(REG_CONTROL, FLAG_CONTROL_IOC_IRQ_EN, dev, buf);
}

ssize_t imsar_xdma_sysfs_control_irq_ioc_en_store(struct device *dev, struct device_attribute *attr, const char *buf,
                                                  size_t size)
{
	return imsar_xdma_sysfs_register_set_mask(REG_CONTROL, FLAG_CONTROL_IOC_IRQ_EN, dev, buf, size);
}

ssize_t imsar_xdma_sysfs_control_irq_err_en_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return imsar_xdma_sysfs_register_query_mask(REG_CONTROL, FLAG_CONTROL_ERR_IRQ_EN, dev, buf);
}

ssize_t imsar_xdma_sysfs_control_irq_err_en_store(struct device *dev, struct device_attribute *attr, const char *buf,
                                                  size_t size)
{
	return imsar_xdma_sysfs_register_set_mask(REG_CONTROL, FLAG_CONTROL_ERR_IRQ_EN, dev, buf, size);
}

ssize_t imsar_xdma_sysfs_reg_control_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return imsar_xdma_sysfs_register_show("0x%08x\n", REG_CONTROL, dev, buf);
}

ssize_t imsar_xdma_sysfs_reg_control_store(struct device *dev, struct device_attribute *attr, const char *buf,
                                           size_t size)
{
	return imsar_xdma_sysfs_register_store(REG_CONTROL, dev, buf, size);
}

ssize_t imsar_xdma_sysfs_reg_status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return imsar_xdma_sysfs_register_show("0x%08x\n", REG_STATUS, dev, buf);
}

ssize_t imsar_xdma_sysfs_reg_status_store(struct device *dev, struct device_attribute *attr, const char *buf,
                                          size_t size)
{
	return imsar_xdma_sysfs_register_store(REG_STATUS, dev, buf, size);
}

ssize_t imsar_xdma_sysfs_reg_address_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return imsar_xdma_sysfs_register_show("0x%08x\n", REG_ADDR_LSB, dev, buf);
}

ssize_t imsar_xdma_sysfs_reg_address_store(struct device *dev, struct device_attribute *attr, const char *buf,
                                           size_t size)
{
	return imsar_xdma_sysfs_register_store(REG_ADDR_LSB, dev, buf, size);
}

ssize_t imsar_xdma_sysfs_reg_length_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return imsar_xdma_sysfs_register_show("%u\n", REG_LENGTH, dev, buf);
}

ssize_t imsar_xdma_sysfs_reg_length_store(struct device *dev, struct device_attribute *attr, const char *buf,
                                          size_t size)
{
	return imsar_xdma_sysfs_register_store(REG_LENGTH, dev, buf, size);
}