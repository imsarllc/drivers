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

#include "version.h"

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

#include "imsar-xdma-defs.h"
#include "imsar-xdma-ops.h"
#include "imsar-xdma-sysfs.h"

MODULE_AUTHOR("IMSAR, LLC. Embedded Team <embedded@imsar.com>");
MODULE_DESCRIPTION("IMSAR cyclic driver for Xilinx DMA core");
MODULE_LICENSE("GPL v2");
MODULE_VERSION(GIT_DESCRIBE);


// ------------------------------------------------------------------
// Forward function declarations
// ------------------------------------------------------------------

// Platform device operations
static int imsar_xdma_probe(struct platform_device *pdev);
static int imsar_xdma_remove(struct platform_device *pdev);

// Module operations
static int __init imsar_xdma_init(void);
static void __exit imsar_xdma_exit(void);

// File operations
static int imsar_xdma_file_open(struct inode *inode, struct file *file);
static int imsar_xdma_file_release(struct inode *inode, struct file *file);
static ssize_t imsar_xdma_file_write(struct file *file, const char __user *buf, size_t bytes, loff_t *ppos);
static ssize_t imsar_xdma_file_read(struct file *file, char __user *buf, size_t bytes, loff_t *off);
static unsigned int imsar_xdma_file_poll(struct file *file, poll_table *wait);
static long imsar_xdma_file_ioctl(struct file *file, unsigned int request, unsigned long arg);

// File helpers
static void imsar_xdma_file_init(imsar_xdma_file_t *file_data, imsar_xdma_channel_t *channel);
static ssize_t imsar_xdma_file_copy_transfer(imsar_xdma_channel_t *channel_data, char __user *buf, size_t bytes,
                                             unsigned int requested_transfer_id);

// Channel consumer functions
static int imsar_xdma_channel_consumer_add(imsar_xdma_channel_t *channel, imsar_xdma_file_t *file_data);
static int imsar_xdma_channel_consumer_remove(imsar_xdma_channel_t *channel, imsar_xdma_file_t *file_data);
static void imsar_xdma_channel_notify_consumers(imsar_xdma_channel_t *channel);

// Device character device operations
static int imsar_xdma_chardev_create(imsar_xdma_dev_t *device_data);
static void imsar_xdma_chardev_destroy(imsar_xdma_dev_t *device_data);

// Channel character device operations
static int imsar_xdma_channel_chardev_create(imsar_xdma_channel_t *channel);
static void imsar_xdma_channel_chardev_destroy(imsar_xdma_channel_t *channel);

// Transfer operations
static void imsar_xdma_channel_setup_transfer(imsar_xdma_channel_t *channel, unsigned int transfer_id);

// Channel buffer metadata
static void imsar_xdma_buffer_meta_init(imsar_xdma_buffer_meta_t *data, unsigned int buffer_size,
                                        unsigned int buffer_index);
static imsar_xdma_buffer_meta_t *imsar_xdma_buffer_meta(imsar_xdma_channel_t *channel, unsigned int transfer_id);
static int imsar_xdma_buffer_alloc(imsar_xdma_channel_t *channel);
static void imsar_xdma_buffer_free(imsar_xdma_channel_t *channel);

// Interrupt handler
static irqreturn_t imsar_xdma_handle_irq(int num, void *channel_data);

// Device operations
static int imsar_xdma_device_data_init(struct platform_device *platform_device, imsar_xdma_dev_t *device_data);
static int imsar_xdma_device_parse_dt(imsar_xdma_dev_t *device_data);

// Channel operations
static imsar_xdma_channel_t *imsar_xdma_channel_create(imsar_xdma_dev_t *device_data, struct device_node *channel_node,
                                                    unsigned int channel_index);
static void imsar_xdma_channel_destroy(imsar_xdma_channel_t *channel_data);
static int imsar_xdma_channel_parse_dt(imsar_xdma_channel_t *channel_data);

// Device class attributes

// ------------------------------------------------------------------
// Function definitions
// ------------------------------------------------------------------

module_init(imsar_xdma_init);
module_exit(imsar_xdma_exit);

// ------------------------------------------------------------------
// Static variables
// ------------------------------------------------------------------
static struct class *s_device_class = NULL;

static const struct of_device_id imsar_xdma_device_table[] = {
    {
        .compatible = "imsar,xdma-simple",
    },
    {},
};

static struct platform_driver imsar_xdma_driver = {
    .driver =
        {
            .name = "imsar_xdma",
            .owner = THIS_MODULE,
            .of_match_table = imsar_xdma_device_table,
        },
    .probe = imsar_xdma_probe,
    .remove = imsar_xdma_remove,
};

static struct file_operations imsar_xdma_fops = {
    .owner = THIS_MODULE,
    .open = imsar_xdma_file_open,
    .release = imsar_xdma_file_release,
    .write = imsar_xdma_file_write,
    .read = imsar_xdma_file_read,
    .poll = imsar_xdma_file_poll,
    .llseek = noop_llseek,
    .unlocked_ioctl = imsar_xdma_file_ioctl,
};


// ------------------------------------------------------------------
// Function definitions
// ------------------------------------------------------------------

// Platform device operations

static int imsar_xdma_probe(struct platform_device *platform_device)
{
	int rc;
	imsar_xdma_dev_t *device_data;
	struct device_node *child_node;
	int channel_index;
	imsar_xdma_channel_t *channel;

	dev_dbg(&platform_device->dev, "imsar_xdma_probe");

	dev_info(&platform_device->dev, "IMSAR Xilinx DMA driver version: %s (%s)\n", GIT_DESCRIBE, BUILD_DATE);

	// Allocate and initialize device data
	device_data = devm_kzalloc(&platform_device->dev, sizeof(imsar_xdma_dev_t), GFP_KERNEL);
	if (device_data == NULL)
	{
		return -ENOMEM;
	}

	// Initialize device data fields
	rc = imsar_xdma_device_data_init(platform_device, device_data);
	if (rc < 0)
	{
		return rc;
	}

	// Parse device-level device tree options
	rc = imsar_xdma_device_parse_dt(device_data);
	if (rc < 0)
	{
		return rc;
	}

	// Map the device IO memory into the kernel virtual memory space
	device_data->regs = devm_platform_ioremap_resource(platform_device, 0);
	if (IS_ERR(device_data->regs))
	{
		return PTR_ERR(device_data->regs);
	}

	// Reset the device (reset is not instantaneous)
	imsar_xdma_reset(device_data);

	// Create character device region big enough for channels
	rc = imsar_xdma_chardev_create(device_data);
	if (rc)
	{
		return rc;
	}

	// Create device channels
	// Iterate through each child node
	channel_index = 0;
	for_each_child_of_node(platform_device->dev.of_node, child_node)
	{
		if (of_device_is_compatible(child_node, "imsar,xdma-channel"))
		{
			if (channel_index == IMSAR_XDMA_MAX_CHANNELS)
			{
				dev_err(&platform_device->dev, "Ignoring channel %pOF because max channels has been reached\n",
				        child_node);
				continue;
			}
			channel = imsar_xdma_channel_create(device_data, child_node, channel_index);
			if (IS_ERR(channel))
			{
				rc = PTR_ERR(channel);
				goto channel_fail;
			}
			device_data->channels[channel_index] = channel;
		}
		channel_index++;
	}

	if (channel_index == 0)
	{
		dev_err(&platform_device->dev, "no \"imsar,xdma-channel\" compatible child nodes\n");
	}
	else
	{
		dev_dbg(&platform_device->dev, "created %u channels\n", channel_index);
	}

	return 0;

channel_fail:
	imsar_xdma_remove(platform_device);

	return 0;
}

static int imsar_xdma_remove(struct platform_device *platform_device)
{
	int channel_index;
	imsar_xdma_dev_t *device_data = dev_get_drvdata(&platform_device->dev);

	dev_dbg(&platform_device->dev, "imsar_xdma_remove");

	for (channel_index = 0; channel_index < IMSAR_XDMA_MAX_CHANNELS; channel_index++)
	{
		if (device_data->channels[channel_index])
		{
			imsar_xdma_channel_destroy(device_data->channels[channel_index]);
		}
	}

	imsar_xdma_chardev_destroy(device_data);

	return 0;
}

// Module operations

static int __init imsar_xdma_init(void)
{
	// Create device class
	s_device_class = class_create(THIS_MODULE, IMSAR_XDMA_DRIVER_NAME);
	if (IS_ERR(s_device_class))
	{
		return PTR_ERR(s_device_class);
	}

	// Register device attribute groups
	s_device_class->dev_groups = imsar_xdma_sysfs_attr_groups;

	// Register as platform driver
	return platform_driver_register(&imsar_xdma_driver);
}

static void __exit imsar_xdma_exit(void)
{
	// Unregister as platform driver
	platform_driver_unregister(&imsar_xdma_driver);

	// Destroy the device class
	if (s_device_class)
	{
		class_destroy(s_device_class);
	}
}


// File operations

static int imsar_xdma_file_open(struct inode *inode, struct file *file)
{
	imsar_xdma_dev_t *xdma_device;
	imsar_xdma_channel_t *channel_data;
	imsar_xdma_file_t *file_data;
	unsigned int channel_index;
	int is_first_consumer;

	channel_index = iminor(inode);
	xdma_device = container_of(inode->i_cdev, imsar_xdma_dev_t, char_dev);
	channel_data = xdma_device->channels[channel_index];

	file_data = kzalloc(sizeof(imsar_xdma_file_t), GFP_KERNEL);
	if (file_data == NULL)
	{
		return -ENOMEM;
	}
	file->private_data = file_data;

	imsar_xdma_file_init(file_data, channel_data);

	is_first_consumer = imsar_xdma_channel_consumer_add(channel_data, file_data);
	if (is_first_consumer)
	{
		imsar_xdma_chan_irq_ack(channel_data); // clear any pending IRQs
		imsar_xdma_chan_start(channel_data);
		imsar_xdma_channel_setup_transfer(channel_data, channel_data->in_progress_transfer_id);
		imsar_xdma_chan_irq_enable(channel_data);
	}

	return 0;
}

static int imsar_xdma_file_release(struct inode *inode, struct file *file)
{
	imsar_xdma_file_t *file_data;
	imsar_xdma_channel_t *channel_data;
	int was_last_consumer;

	file_data = (imsar_xdma_file_t *)file->private_data;
	channel_data = file_data->channel;

	was_last_consumer = imsar_xdma_channel_consumer_remove(channel_data, file_data);
	if (was_last_consumer)
	{
		imsar_xdma_chan_stop(channel_data);
		imsar_xdma_chan_irq_disable(channel_data);
	}

	kfree(file_data);

	return 0;
}

static ssize_t imsar_xdma_file_write(struct file *file, const char __user *buf, size_t bytes, loff_t *ppos)
{
	return -EPERM;
}

static ssize_t imsar_xdma_file_copy_transfer(imsar_xdma_channel_t *channel_data, char __user *buf, size_t bytes,
                                             unsigned int requested_transfer_id)
{
	int status;
	imsar_xdma_buffer_meta_t *buffer_info;
	unsigned int buffer_transfer_id;
	unsigned int buffer_length;
	unsigned int actual_bytes;

	// Get the transfer ID out of the buffer info
	buffer_info = imsar_xdma_buffer_meta(channel_data, requested_transfer_id);
	buffer_transfer_id = buffer_info->transfer_id;
	buffer_length = buffer_info->length;

	// Check to make sure the buffer contains the transfer we are looking for
	if (buffer_transfer_id != requested_transfer_id)
	{
		dev_warn(channel_data->xdma_device->device, "%s: bad/old buffer transfer ID; des=%u, act=%u\n",
		         channel_data->name, requested_transfer_id, buffer_transfer_id);
		return -EINVAL;
	}

	// Copy the data to the user
	actual_bytes = (bytes > buffer_length) ? buffer_length : bytes;
	status = copy_to_user(buf, channel_data->buffer_virt_addr + buffer_info->offset, actual_bytes);
	if (status != 0)
	{
		dev_dbg(channel_data->xdma_device->device, "%s: copy_to_user failed", channel_data->name);
		return -EFAULT;
	}

	// Check the buffer transfer ID again to make sure the data cannot have changed
	buffer_transfer_id = buffer_info->transfer_id;
	if (buffer_transfer_id != requested_transfer_id)
	{
		dev_warn(channel_data->xdma_device->device, "%s: ID changed during copy\n", channel_data->name);
		return -EINVAL;
	}

	return actual_bytes;
}

static ssize_t imsar_xdma_file_read(struct file *file, char __user *buf, size_t bytes, loff_t *off)
{
	int status;
	imsar_xdma_file_t *file_data;
	imsar_xdma_channel_t *channel_data;
	unsigned int last_finished_transfer_id, desired_transfer_id;

	file_data = (imsar_xdma_file_t *)file->private_data;
	channel_data = file_data->channel;

	// If no new transfers are available, wait until one is (or return if non-blocking)
	if (channel_data->last_finished_transfer_id == file_data->last_read_transfer_id)
	{
		if (file->f_flags & O_NONBLOCK)
		{
			return -EAGAIN;
		}
		else
		{
			status = wait_event_interruptible(file_data->file_waitqueue, (channel_data->last_finished_transfer_id >
			                                                              file_data->last_read_transfer_id));
			if (status < 0) // timeout
			{
				return status;
			}
		}
	}

	// Read the most recent transfer ID
	last_finished_transfer_id = channel_data->last_finished_transfer_id;

	if (file_data->last_read_transfer_id + channel_data->buffer_count - 1 <= last_finished_transfer_id)
	{
		dev_dbg(channel_data->xdma_device->device, "%s: file transfer ID is too far behind; fast-forwarding\n",
		        channel_data->name);
		file_data->last_read_transfer_id = last_finished_transfer_id - channel_data->buffer_count + 2;
	}

	desired_transfer_id = file_data->last_read_transfer_id + 1;

	while (desired_transfer_id <= last_finished_transfer_id)
	{
		status = imsar_xdma_file_copy_transfer(channel_data, buf, bytes, desired_transfer_id);
		if (status >= 0)
		{ // Successful read (or error during copy_to_user)
			file_data->last_read_transfer_id = desired_transfer_id;
			return status;
		}

		if (status == -EFAULT)
		{ // bad copy_to_user result
			return status;
		}

		// We errored, but we can try the next transfer
		desired_transfer_id++;
	}

	dev_warn(channel_data->xdma_device->device, "%s: no buffers were available\n", channel_data->name);
	return -EIO;
}

static unsigned int imsar_xdma_file_poll(struct file *file, poll_table *wait)
{
	imsar_xdma_file_t *file_data;
	imsar_xdma_channel_t *channel_data;

	file_data = (imsar_xdma_file_t *)file->private_data;
	channel_data = file_data->channel;

	if (channel_data->last_finished_transfer_id == file_data->last_read_transfer_id)
	{
		// NOTE: this is NOT a blocking call -- this function (imsar_xdma_file_poll)
		// will be called again when the wait queue is posted by the interrupt
		poll_wait(file, &file_data->file_waitqueue, wait);
		return 0;
	}
	else
	{
		return (POLLIN | POLLRDNORM);
	}
}

static long imsar_xdma_file_ioctl(struct file *file, unsigned int request, unsigned long arg)
{
	return -EINVAL;
}

static void imsar_xdma_file_init(imsar_xdma_file_t *file_data, imsar_xdma_channel_t *channel)
{
	file_data->channel = channel;
	file_data->last_read_transfer_id = channel->last_finished_transfer_id;
	init_waitqueue_head(&file_data->file_waitqueue);
	INIT_LIST_HEAD(&file_data->list);
}

static int imsar_xdma_channel_consumer_add(imsar_xdma_channel_t *channel, imsar_xdma_file_t *file_data)
{
	int list_was_empty;

	spin_lock(&channel->consumers_spinlock);
	list_was_empty = list_empty(&channel->consuming_files);
	list_add_tail(&file_data->list, &channel->consuming_files);
	spin_unlock(&channel->consumers_spinlock);
	return list_was_empty;
}

static int imsar_xdma_channel_consumer_remove(imsar_xdma_channel_t *channel, imsar_xdma_file_t *file_data)
{
	int list_is_now_empty;

	spin_lock(&channel->consumers_spinlock);
	list_del(&file_data->list);
	list_is_now_empty = list_empty(&channel->consuming_files);
	spin_unlock(&channel->consumers_spinlock);
	return list_is_now_empty;
}

static void imsar_xdma_channel_notify_consumers(imsar_xdma_channel_t *channel)
{
	unsigned long flags;
	imsar_xdma_file_t *entry;

	spin_lock_irqsave(&channel->consumers_spinlock, flags);
	list_for_each_entry(entry, &channel->consuming_files, list)
	{
		wake_up_interruptible(&entry->file_waitqueue);
	}
	spin_unlock_irqrestore(&channel->consumers_spinlock, flags);
}


// Character device functions

static int imsar_xdma_chardev_create(imsar_xdma_dev_t *device_data)
{
	int rc;

	// Allocate a character device region big enough for the channels of this device
	rc = alloc_chrdev_region(&device_data->char_dev_node, 0, IMSAR_XDMA_MAX_CHANNELS, IMSAR_XDMA_DRIVER_NAME);
	if (rc)
	{
		dev_err(device_data->device, "alloc_chrdev_region failed\n");
		return rc;
	}

	// Initialize the character device data structure
	cdev_init(&device_data->char_dev, &imsar_xdma_fops);
	device_data->char_dev.owner = THIS_MODULE;

	// Add the character device
	rc = cdev_add(&device_data->char_dev, device_data->char_dev_node, IMSAR_XDMA_MAX_CHANNELS);
	if (rc)
	{
		dev_err(device_data->device, "unable to add char device\n");
		goto char_dev_fail;
	}

	return 0;

char_dev_fail:
	unregister_chrdev_region(device_data->char_dev_node, IMSAR_XDMA_MAX_CHANNELS);

	return rc;
}

static void imsar_xdma_chardev_destroy(imsar_xdma_dev_t *device_data)
{
	cdev_del(&device_data->char_dev);
	unregister_chrdev_region(device_data->char_dev_node, IMSAR_XDMA_MAX_CHANNELS);
}

static int imsar_xdma_channel_chardev_create(imsar_xdma_channel_t *channel)
{
	// Create device node
	channel->char_dev_device = device_create(s_device_class, channel->xdma_device->device,              //
	                                      channel->xdma_device->char_dev_node + channel->channel_index, //
	                                      channel, "dma_%s", channel->name);

	if (IS_ERR(channel->char_dev_device))
	{
		dev_err(channel->xdma_device->device, "unable to create the device\n");
		return -ENOMEM;
	}

	return 0;
}

static void imsar_xdma_channel_chardev_destroy(imsar_xdma_channel_t *channel)
{
	if (channel->char_dev_device)
	{
		device_destroy(s_device_class, channel->xdma_device->char_dev_node + channel->channel_index);
		channel->char_dev_device = 0;
	}
}

static void imsar_xdma_channel_setup_transfer(imsar_xdma_channel_t *channel, unsigned int transfer_id)
{
	imsar_xdma_buffer_meta_t *buffer_metadata = imsar_xdma_buffer_meta(channel, transfer_id);

	if (channel->log_transfer_events)
	{
		dev_dbg(channel->xdma_device->device, "%s: setup transfer %u (len %u)\n", channel->name, transfer_id,
		        channel->buffer_size_bytes);
	}

	buffer_metadata->transfer_id = transfer_id;
	buffer_metadata->length = 0;

	imsar_xdma_chan_set_addr_and_len(channel, channel->buffer_bus_addr + buffer_metadata->offset, channel->buffer_size_bytes);

	channel->in_progress_transfer_id = transfer_id;
}

static int imsar_xdma_buffer_alloc(imsar_xdma_channel_t *channel)
{
	int rc;
	int i;

	// Allocate DMA coherent memory that will be shared with user space
	channel->buffer_virt_addr = dmam_alloc_coherent(channel->xdma_device->device,                    // dev
	                                             channel->buffer_size_bytes * channel->buffer_count, // size
	                                             &channel->buffer_bus_addr,                       // dma_handle (out)
	                                             GFP_KERNEL);                                  // flags

	if (!channel->buffer_virt_addr)
	{
		dev_err(channel->xdma_device->device, "DMA allocation error\n");
		return -ENOMEM;
	}

	dev_dbg(channel->xdma_device->device, "alloc DMA memory; VAddr: %px, BAddr: %px, size: %u\n", channel->buffer_virt_addr,
	        (void *)channel->buffer_bus_addr, channel->buffer_size_bytes * channel->buffer_count);

	channel->buffer_metadata =
	    devm_kzalloc(channel->xdma_device->device, sizeof(imsar_xdma_buffer_meta_t) * channel->buffer_count, GFP_KERNEL);

	rc = 0;
	if (!channel->buffer_metadata)
	{
		dev_err(channel->xdma_device->device, "buffer status allocation error\n");
		rc = -ENOMEM;
		goto buffer_alloc_fail;
	}

	for (i = 0; i < channel->buffer_count; i++)
	{
		imsar_xdma_buffer_meta_init(&channel->buffer_metadata[i], channel->buffer_size_bytes, i);
	}

	return 0;

buffer_alloc_fail:
	imsar_xdma_buffer_free(channel);

	return rc;
}

static void imsar_xdma_buffer_free(imsar_xdma_channel_t *channel)
{
	if (channel->buffer_virt_addr)
	{
		dev_dbg(channel->xdma_device->device, "free DMA memory; VAddr: %px, BAddr: %px\n", channel->buffer_virt_addr,
		        (void *)channel->buffer_bus_addr);

		dmam_free_coherent(channel->xdma_device->device, channel->buffer_size_bytes * channel->buffer_count,
		                   channel->buffer_virt_addr, channel->buffer_bus_addr);

		channel->buffer_virt_addr = 0;
		channel->buffer_bus_addr = 0;
	}

	if (channel->buffer_metadata)
	{
		devm_kfree(channel->xdma_device->device, channel->buffer_metadata);
		channel->buffer_metadata = 0;
	}
}

static void imsar_xdma_buffer_meta_init(imsar_xdma_buffer_meta_t *data, unsigned int buffer_size,
                                        unsigned int buffer_index)
{
	data->transfer_id = 0;
	data->length = 0;
	data->offset = buffer_size * buffer_index;
}

static imsar_xdma_buffer_meta_t *imsar_xdma_buffer_meta(imsar_xdma_channel_t *channel, unsigned int transfer_id)
{
	unsigned int buffer_index = transfer_id % channel->buffer_count;
	return &channel->buffer_metadata[buffer_index];
}

// Interrupt handlers

static irqreturn_t imsar_xdma_handle_irq(int num, void *channel_data)
{
	imsar_xdma_channel_t *channel;
	u32 status;
	unsigned int in_progress_transfer_id;
	unsigned int next_transfer_id;
	imsar_xdma_buffer_meta_t *buffer_metadata;
	u32 length = 0;

	channel = channel_data;
	in_progress_transfer_id = channel->in_progress_transfer_id;
	status = imsar_xdma_chan_reg_read(channel, REG_STATUS);

	if (!(status & FLAG_STATUS_ALL_IRQ)) // Not our interrupt
	{
		return IRQ_NONE;
	}

	imsar_xdma_chan_irq_ack(channel);

	if (status & FLAG_STATUS_ERR_IRQ)
	{
		dev_warn(channel->xdma_device->device, "%s: Transfer error with status 0x%08x\n", channel->name, status);

		if (status & FLAG_STATUS_DMA_SLV_ERR)
		{
			dev_warn(channel->xdma_device->device, "%s: DMA Slave Error\n", channel->name);
		}

		if (status & FLAG_STATUS_DMA_DEC_ERR)
		{
			dev_warn(channel->xdma_device->device, "%s: DMA Decode Error\n", channel->name);
		}

		if (status & FLAG_STATUS_DMA_INT_ERR)
		{
			dev_warn(channel->xdma_device->device, "%s: DMA Internal Error\n", channel->name);
		}

		// Clear all error flags
		// imsar_xdma_chan_reg_bit_clr_set(channel, REG_STATUS, FLAG_STATUS_DMA_ALL_ERRS, 0);

		// Restart the channel
		imsar_xdma_chan_start(channel);

		length = 0;
		next_transfer_id = in_progress_transfer_id;
	}
	else if (status & FLAG_STATUS_IOC_IRQ)
	{
		if (!(status & FLAG_STATUS_IDLE))
		{
			dev_warn(channel->xdma_device->device, "%s: got completion interrupt, but channel is not idle!\n", channel->name);
		}

		length = imsar_xdma_chan_read_len(channel);

		if (channel->log_transfer_events)
		{
			dev_dbg(channel->xdma_device->device, "%s: finished transfer %u (len %u)\n", channel->name,
			        in_progress_transfer_id, length);
		}

		next_transfer_id = in_progress_transfer_id + 1;
	}

	// Set the address and length for the next transfer (this allows the hardware to continue)
	imsar_xdma_channel_setup_transfer(channel, next_transfer_id);

	if (length > 0)
	{
		buffer_metadata = imsar_xdma_buffer_meta(channel, in_progress_transfer_id);
		buffer_metadata->length = length;

		channel->last_finished_transfer_id = in_progress_transfer_id;

		imsar_xdma_channel_notify_consumers(channel);
	}

	return IRQ_HANDLED;
}

// Device functions
static int imsar_xdma_device_data_init(struct platform_device *platform_device, imsar_xdma_dev_t *device_data)
{
	dev_set_drvdata(&platform_device->dev, device_data);
	device_data->platform_device = platform_device;
	device_data->device = &platform_device->dev;
	device_data->log_register_access = false;
	return 0;
}

static int imsar_xdma_device_parse_dt(imsar_xdma_dev_t *device_data)
{
	int rc;

	// Read the name of the device
	rc = device_property_read_string(device_data->device, "imsar,name", &device_data->name);
	if (rc)
	{
		device_data->name = device_data->device->of_node->name;
		dev_warn(device_data->device, "missing property: imsar,name (defaulted to %s)\n", device_data->name);
	}
	dev_dbg(device_data->device, "name: %s\n", device_data->name);

	return 0;
}

// Channel operations
static imsar_xdma_channel_t *imsar_xdma_channel_create(imsar_xdma_dev_t *device_data, struct device_node *channel_node,
                                                    unsigned int channel_index)
{
	int rc;
	imsar_xdma_channel_t *channel_data;

	channel_data = devm_kzalloc(device_data->device, sizeof(imsar_xdma_channel_t), GFP_KERNEL);
	if (!channel_data)
	{
		return ERR_PTR(-ENOMEM);
	}

	channel_data->xdma_device = device_data;
	channel_data->device_node = channel_node;

	channel_data->channel_index = channel_index;
	channel_data->in_progress_transfer_id = 1;
	channel_data->last_finished_transfer_id = 0;
	channel_data->log_transfer_events = false;

	spin_lock_init(&channel_data->consumers_spinlock);
	INIT_LIST_HEAD(&channel_data->consuming_files);

	// Parse channel device tree node
	rc = imsar_xdma_channel_parse_dt(channel_data);
	if (rc)
	{
		return ERR_PTR(rc);
	}

	// Allocate channel DMA and metadata buffers
	rc = imsar_xdma_buffer_alloc(channel_data);
	if (rc)
	{
		return ERR_PTR(rc);
	}

	// Request interrupt
	rc = devm_request_irq(device_data->device, channel_data->irq, imsar_xdma_handle_irq, IRQF_SHARED, "imsar-xdma",
	                      channel_data);
	if (rc)
	{
		dev_err(device_data->device, "unable to request IRQ %d\n", channel_data->irq);
		return ERR_PTR(rc);
	}

	// Create character device for channel
	rc = imsar_xdma_channel_chardev_create(channel_data);
	if (rc)
	{
		return ERR_PTR(rc);
	}

	return channel_data;
}

static void imsar_xdma_channel_destroy(imsar_xdma_channel_t *channel_data)
{
	imsar_xdma_channel_chardev_destroy(channel_data);
	imsar_xdma_buffer_free(channel_data);
}

static int imsar_xdma_channel_parse_dt(imsar_xdma_channel_t *channel_data)
{
	int rc;
	const char *dir;
	struct device *dev;
	struct device_node *dev_node;

	dev = channel_data->xdma_device->device;
	dev_node = channel_data->device_node;

	// imsar,name
	rc = of_property_read_string(dev_node, "imsar,name", &channel_data->name);
	if (rc)
	{
		channel_data->name = dev_node->name;
		dev_warn(dev, "missing optional property: imsar,name (defaulted to %s)\n", channel_data->name);
	}

	// imsar,direction
	rc = of_property_read_string(dev_node, "imsar,direction", &dir);
	if (rc)
	{
		dev_err(dev, "missing required property: imsar,direction\n");
		return rc;
	}

	if (strcmp(dir, "s2mm") == 0)
	{
		channel_data->direction = IMSAR_XDMA_DIR_S2MM;
	}
	else if (strcmp(dir, "mm2s") == 0)
	{
		dev_err(dev, "mm2s is not currently supported!");
		channel_data->direction = IMSAR_XDMA_DIR_MM2S;
	}
	else
	{
		dev_err(dev, "invalid imsar,direction: %s (must be s2mm or mm2s)\n", dir);
		return -EINVAL;
	}

	// imsar,buffer-count
	rc = of_property_read_u32(dev_node, "imsar,buffer-count", &channel_data->buffer_count);
	if (rc)
	{
		dev_err(dev, "Missing required property: imsar,buffer-count\n");
		return rc;
	}

	// imsar,buffer-size-bytes
	rc = of_property_read_u32(dev_node, "imsar,buffer-size-bytes", &channel_data->buffer_size_bytes);
	if (rc)
	{
		dev_err(dev, "Missing required property: imsar,buffer-size-bytes\n");
		return rc;
	}

	// reg
	rc = of_property_read_u32(dev_node, "reg", &channel_data->reg_offset);
	if (rc)
	{
		dev_err(dev, "Missing required property: reg\n");
		return rc;
	}

	// interrupts
	channel_data->irq = of_irq_get(dev_node, 0); // first interrupt
	if (channel_data->irq < 0)
	{
		return dev_err_probe(dev, channel_data->irq, "failed to get irq\n");
	}

	dev_info(dev, "channel %s: dir=%u, reg_offset=0x%x, irq=%u, buffer count=%u, bytes=%u", channel_data->name,
	         channel_data->direction, channel_data->reg_offset, channel_data->irq, channel_data->buffer_count,
	         channel_data->buffer_size_bytes);

	return 0;
}
