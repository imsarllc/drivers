/**
 * IMSAR User Space DMA driver
 *
 * Copyright (C) 2024 IMSAR, LLC.
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
 */

#include "version.h"

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/workqueue.h>

#include "imdma.h"

#define IMDMA_DRIVER_NAME "imdma"
#define IMDMA_TIMEOUT_MS_MAX 30000

MODULE_AUTHOR("IMSAR, LLC. Embedded Team <embedded@imsar.com>");
MODULE_DESCRIPTION("IMSAR User Space DMA driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION(GIT_DESCRIBE);

// ------------------------------------------------------------------
// Data structure definitions
// ------------------------------------------------------------------

struct imdma_device;

enum imdma_buffer_state
{
	IMDMA_BUFFER_UNDEF,       // Unknown state
	IMDMA_BUFFER_FREE,        // Available
	IMDMA_BUFFER_RESERVED,    // Reserved, but no transfer started yet
	IMDMA_BUFFER_IN_PROGRESS, // Transfer started, but not completed or abandoned (timed out) yet
	IMDMA_BUFFER_DONE,        // Transfer finished (or errored), waiting for user space to release
};

struct imdma_buffer_status
{
	unsigned int buffer_index;
	unsigned int buffer_offset;

	enum imdma_buffer_state buffer_state;
	spinlock_t buffer_state_spinlock;

	unsigned int length_bytes;
	struct completion cmp;
	dma_cookie_t cookie;
	dma_addr_t dma_handle;
	struct scatterlist sg_list;

	struct imdma_device *device_data; // used by completion callback to access device
};

struct imdma_device
{
	// DT properties
	const char *device_name;               // name
	const char *dma_channel_name;          // dma-names
	unsigned int buffer_count;             // imsar,buffer-count
	unsigned int buffer_size_bytes;        // imsar,buffer-size-bytes
	enum dma_transfer_direction direction; // imsar,direction
	unsigned int default_timeout_ms;       // imsar,default-timeout-ms
	unsigned int address_width;            // 1-32 bits

	// Device
	struct device *device;

	// Usage counter
	unsigned int usage_count; // how many processes have the device open
	struct mutex usage_count_mutex;

	// DMA and buffer
	struct dma_chan *dma_channel;
	unsigned char *buffer_virtual_address; // user/kernel space shared buffer (DMA coherent)
	dma_addr_t buffer_bus_address;
	struct imdma_buffer_status *buffer_statuses;

	// Character device
	dev_t char_dev_node;
	struct cdev char_dev;
	struct device *char_dev_device;
};

// ------------------------------------------------------------------
// Forward function declarations
// ------------------------------------------------------------------

// File operations
static int imdma_open(struct inode *ino, struct file *file);
static int imdma_release(struct inode *ino, struct file *file);
static int imdma_mmap(struct file *file_p, struct vm_area_struct *vma);
static long imdma_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

// ioctl implementations
static long imdma_ioctl_buffer_get_spec(struct imdma_device *device_data, unsigned long arg);
static long imdma_ioctl_buffer_reserve(struct imdma_device *device_data, unsigned long arg);
static long imdma_ioctl_buffer_release(struct imdma_device *device_data, unsigned long arg);
static long imdma_ioctl_transfer_start(struct imdma_device *device_data, unsigned long arg);
static long imdma_ioctl_transfer_finish(struct imdma_device *device_data, unsigned long arg);

// Platform device operations
static int imdma_probe(struct platform_device *pdev);
static int imdma_remove(struct platform_device *pdev);

// Module operations
static int __init imdma_init(void);
static void __exit imdma_exit(void);

// Internal helper functions
static int imdma_buffer_change_state_if(struct imdma_buffer_status *status, enum imdma_buffer_state prev_state,
                                        enum imdma_buffer_state new_state);
static int imdma_transfer_start(struct imdma_device *device_data, struct imdma_transfer_start_spec *spec);
static int imdma_transfer_finish(struct imdma_device *device_data, struct imdma_transfer_finish_spec *spec);
static void imdma_transfer_complete_callback(void *buffer_status);
static void imdma_buffer_status_init(struct imdma_device *device_data, struct imdma_buffer_status *status,
                                     unsigned int buffer_index);
static int imdma_buffer_alloc(struct imdma_device *device_data);
static void imdma_buffer_free(struct imdma_device *device_data);
static int imdma_parse_dt(struct imdma_device *device_data);
static int imdma_char_dev_create(struct imdma_device *device_data);
static void imdma_char_dev_destroy(struct imdma_device *device_data);
ssize_t imdma_name_show(struct device *dev, struct device_attribute *attr, char *buf);

// ------------------------------------------------------------------
// Static variables
// ------------------------------------------------------------------
static struct class *s_device_class = NULL;

static struct file_operations imdma_file_ops = {
    .owner = THIS_MODULE,          //
    .open = imdma_open,            //
    .release = imdma_release,      //
    .unlocked_ioctl = imdma_ioctl, //
    .mmap = imdma_mmap             //
};

static const struct of_device_id imdma_device_table[] = {
    {
        .compatible = "imsar,dma-channel",
    },
    {},
};

static struct platform_driver imdma_driver = {
    .driver =
        {
            .name = "imdma",
            .owner = THIS_MODULE,
            .of_match_table = imdma_device_table,
        },
    .probe = imdma_probe,
    .remove = imdma_remove,
};

MODULE_DEVICE_TABLE(of, imdma_device_table);

// Attributes
static DEVICE_ATTR(name, S_IRUGO, imdma_name_show, NULL);
static struct attribute *imdma_attrs[] = {
    &dev_attr_name.attr,
    NULL,
};
static struct attribute_group imdma_attr_group = {
    .attrs = imdma_attrs,
};
static const struct attribute_group *imdma_attr_groups[] = {
    &imdma_attr_group,
    NULL,
};

// ------------------------------------------------------------------
// Function definitions
// ------------------------------------------------------------------

module_init(imdma_init);
module_exit(imdma_exit);

// File operations

static int imdma_open(struct inode *ino, struct file *file)
{
	int rc;
	struct imdma_device *device_data = container_of(ino->i_cdev, struct imdma_device, char_dev);

	dev_dbg(device_data->device, "imdma_open(...)");

	file->private_data = device_data;

	rc = mutex_lock_interruptible(&device_data->usage_count_mutex);
	if (rc)
	{
		dev_dbg(device_data->device, "open was interrupted");
		return -EINTR;
	}

	rc = 0;

	// Allocate buffer on first user
	if (device_data->usage_count == 0)
	{
		rc = imdma_buffer_alloc(device_data);
		if (rc)
		{
			dev_err(device_data->device, "imdma_buffer_alloc error; rc=%d\n", rc);
		}
	}
	else
	{
		dev_warn(device_data->device, "Device is already opened by %d processes!", device_data->usage_count);
	}

	// Increment the usage count
	device_data->usage_count++;

	mutex_unlock(&device_data->usage_count_mutex);

	return rc;
}

static int imdma_release(struct inode *ino, struct file *file)
{
	int rc;
	struct imdma_device *device_data = (struct imdma_device *)file->private_data;

	dev_dbg(device_data->device, "imdma_release(...)");

	rc = mutex_lock_interruptible(&device_data->usage_count_mutex);

	// Decrement usage count
	device_data->usage_count--;

	// If there are no more users, free the buffers
	if (device_data->usage_count == 0)
	{
		dmaengine_terminate_sync(device_data->dma_channel); // make sure all transfers are finished
		imdma_buffer_free(device_data);
	}

	mutex_unlock(&device_data->usage_count_mutex);

	return 0;
}

static int imdma_mmap(struct file *file_p, struct vm_area_struct *vma)
{
	struct imdma_device *device_data = (struct imdma_device *)file_p->private_data;

	dev_dbg(device_data->device, "imdma_mmap(...)");

	// TODO: validate requested range?????

	return dma_mmap_coherent(device_data->device,                 // dev
	                         vma,                                 // vma
	                         device_data->buffer_virtual_address, // cpu_addr
	                         device_data->buffer_bus_address,     // handle
	                         vma->vm_end - vma->vm_start          // size
	);
}

static long imdma_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct imdma_device *device_data = (struct imdma_device *)file->private_data;

	// dev_dbg(device_data->device, "imdma_ioctl(..., %u, %px)", cmd, (void *)arg);

	switch (cmd)
	{
	case IMDMA_BUFFER_GET_SPEC:
		return imdma_ioctl_buffer_get_spec(device_data, arg);
	case IMDMA_BUFFER_RESERVE:
		return imdma_ioctl_buffer_reserve(device_data, arg);
	case IMDMA_BUFFER_RELEASE:
		return imdma_ioctl_buffer_release(device_data, arg);
	case IMDMA_TRANSFER_START:
		return imdma_ioctl_transfer_start(device_data, arg);
	case IMDMA_TRANSFER_FINISH:
		return imdma_ioctl_transfer_finish(device_data, arg);
	default:
		dev_warn(device_data->device, "unrecognized ioctl cmd: %u", cmd);
		return -EINVAL;
	}
}

// ioctl implementations

static long imdma_ioctl_buffer_get_spec(struct imdma_device *device_data, unsigned long arg)
{
	struct imdma_buffer_spec buffer_spec;

	dev_dbg(device_data->device, "imdma_ioctl_buffer_get_spec(..., %px)", (void *)arg);

	buffer_spec.count = device_data->buffer_count;
	buffer_spec.size_bytes = device_data->buffer_size_bytes;

	if (copy_to_user((struct imdma_buffer_spec *)arg, &buffer_spec, sizeof(buffer_spec)))
	{
		dev_warn(device_data->device, "copy_to_user failed");
		return -EINVAL;
	}

	return 0;
}

static int imdma_buffer_change_state_if(struct imdma_buffer_status *status, enum imdma_buffer_state prev_state,
                                        enum imdma_buffer_state new_state)
{
	int rc;
	spin_lock(&status->buffer_state_spinlock);
	if (status->buffer_state == prev_state || prev_state == IMDMA_BUFFER_UNDEF)
	{
		status->buffer_state = new_state;
		rc = 1;
	}
	else
	{
		rc = 0;
	}
	spin_unlock(&status->buffer_state_spinlock);
	return rc;
}

static long imdma_ioctl_buffer_reserve(struct imdma_device *device_data, unsigned long arg)
{
	unsigned int buffer_idx;
	struct imdma_buffer_status *status;
	struct imdma_buffer_reserve_spec spec;
	int rc = -ENOBUFS;

	for (buffer_idx = 0; buffer_idx < device_data->buffer_count; buffer_idx++)
	{
		status = &device_data->buffer_statuses[buffer_idx];

		if (imdma_buffer_change_state_if(status, IMDMA_BUFFER_FREE, IMDMA_BUFFER_RESERVED))
		{
			spec.buffer_index = status->buffer_index;
			spec.offset_bytes = status->buffer_offset;
			rc = 0;
			break;
		}
	}

	if (rc != 0)
	{
		return rc;
	}

	if (copy_to_user((struct imdma_buffer_reserve_spec *)arg, &spec, sizeof(spec)))
	{
		status = &device_data->buffer_statuses[spec.buffer_index];
		imdma_buffer_change_state_if(status, IMDMA_BUFFER_RESERVED, IMDMA_BUFFER_FREE);
		dev_warn(device_data->device, "copy_to_user failed");
		return -EINVAL;
	}

	return rc;
}

static long imdma_ioctl_buffer_release(struct imdma_device *device_data, unsigned long arg)
{
	struct imdma_buffer_status *status;
	struct imdma_buffer_release_spec spec;
	struct imdma_transfer_finish_spec wait_spec;
	int rc = 0;

	if (copy_from_user(&spec, (struct imdma_buffer_release_spec *)arg, sizeof(spec)))
	{
		dev_warn(device_data->device, "copy_from_user failed");
		return -EINVAL;
	}

	if (spec.buffer_index >= device_data->buffer_count)
	{
		dev_warn(device_data->device, "buffer index out of bounds: %u (max %u)", spec.buffer_index,
		         device_data->buffer_count - 1);
		return -ENOENT;
	}

	status = &device_data->buffer_statuses[spec.buffer_index];

	spin_lock(&status->buffer_state_spinlock);
	if (status->buffer_state == IMDMA_BUFFER_RESERVED || status->buffer_state == IMDMA_BUFFER_DONE)
	{
		status->buffer_state = IMDMA_BUFFER_FREE;
		rc = 0;
	}
	else if (status->buffer_state == IMDMA_BUFFER_FREE)
	{
		rc = -EPERM;
	}
	else if (status->buffer_state == IMDMA_BUFFER_IN_PROGRESS)
	{
		spin_unlock(&status->buffer_state_spinlock);
		wait_spec.buffer_index = spec.buffer_index;
		wait_spec.timeout_ms = 0; // will use default_timeout_ms
		rc = imdma_transfer_finish(device_data, &wait_spec);
		if (rc)
		{
			dev_emerg(device_data->device, "Transfer on buffer %d never finished. Giving up!\n", spec.buffer_index);
		}
		spin_lock(&status->buffer_state_spinlock);
		status->buffer_state = IMDMA_BUFFER_FREE;
		rc = 0;
	}
	else
	{
		dev_err(device_data->device, "buffer_release: Unhandled buffer state: %u (buffer_index = %u)\n",
		        status->buffer_state, spec.buffer_index);
		rc = -EIO;
	}
	spin_unlock(&status->buffer_state_spinlock);

	return rc;
}

static long imdma_ioctl_transfer_start(struct imdma_device *device_data, unsigned long arg)
{
	int rc;
	struct imdma_transfer_start_spec spec;
	struct imdma_buffer_status *status;

	// dev_dbg(device_data->device, "imdma_ioctl_transfer_start(..., %px)", (void *)arg);

	if (copy_from_user(&spec, (struct imdma_spec *)arg, sizeof(spec)))
	{
		dev_warn(device_data->device, "copy_from_user failed");
		return -EINVAL;
	}

	if (spec.buffer_index >= device_data->buffer_count)
	{
		dev_warn(device_data->device, "buffer index out of bounds: %u (max %u)", spec.buffer_index,
		         device_data->buffer_count - 1);
		return -ENOENT;
	}

	if (spec.length_bytes > device_data->buffer_size_bytes)
	{
		dev_warn(device_data->device, "length_bytes (%u) is greater than buffer size  (%u) ", spec.length_bytes,
		         device_data->buffer_size_bytes);
		return -EOVERFLOW;
	}

	status = &device_data->buffer_statuses[spec.buffer_index];

	spin_lock(&status->buffer_state_spinlock);
	if (status->buffer_state == IMDMA_BUFFER_RESERVED)
	{
		status->buffer_state = IMDMA_BUFFER_IN_PROGRESS;
		rc = imdma_transfer_start(device_data, &spec);
		if (rc)
		{
			dev_warn(device_data->device, "buffer %d failed to start transfer (rc=%d)", spec.buffer_index, rc);
			rc = -EIO;
		}
	}
	else if (status->buffer_state == IMDMA_BUFFER_FREE)
	{
		dev_warn(device_data->device, "buffer %d is not reserved", spec.buffer_index);
		rc = -EPERM;
	}
	else if (status->buffer_state == IMDMA_BUFFER_IN_PROGRESS)
	{
		dev_warn(device_data->device, "buffer %d is already in progress", spec.buffer_index);
		rc = -EALREADY;
	}
	else
	{
		dev_err(device_data->device, "transfer_start: unhandled buffer state: %u (buffer_index = %u)\n",
		        status->buffer_state, spec.buffer_index);
		rc = -EIO;
	}
	spin_unlock(&status->buffer_state_spinlock);

	return rc;
}

static long imdma_ioctl_transfer_finish(struct imdma_device *device_data, unsigned long arg)
{
	int rc;
	struct imdma_transfer_finish_spec spec;
	struct imdma_buffer_status *status;

	// dev_dbg(device_data->device, "imdma_ioctl_transfer_finish(..., %px)", (void *)arg);

	if (copy_from_user(&spec, (struct imdma_transfer_finish_spec *)arg, sizeof(spec)))
	{
		dev_warn(device_data->device, "copy_from_user failed");
		return -EINVAL;
	}

	if (spec.buffer_index >= device_data->buffer_count)
	{
		dev_warn(device_data->device, "buffer index out of bounds: %u (max %u)", spec.buffer_index,
		         device_data->buffer_count - 1);
		return -ENOENT;
	}

	if (spec.timeout_ms >= IMDMA_TIMEOUT_MS_MAX) // 30 seconds max
	{
		dev_warn(device_data->device, "timeout_ms is too large: %u (max %u)", spec.timeout_ms, IMDMA_TIMEOUT_MS_MAX);
		return -EINVAL;
	}

	status = &device_data->buffer_statuses[spec.buffer_index];

	spin_lock(&status->buffer_state_spinlock);
	if (status->buffer_state == IMDMA_BUFFER_IN_PROGRESS || status->buffer_state == IMDMA_BUFFER_DONE)
	{
		spin_unlock(&status->buffer_state_spinlock);
		rc = imdma_transfer_finish(device_data, &spec);
		spin_lock(&status->buffer_state_spinlock);
	}
	else
	{
		dev_err(device_data->device, "transfer_finish: unhandled buffer state: %u (buffer_index = %u)\n",
		        status->buffer_state, spec.buffer_index);
		rc = -EPERM;
	}
	spin_unlock(&status->buffer_state_spinlock);

	if (rc)
	{
		return rc;
	}

	return 0;
}


// Platform device operations

static int imdma_probe(struct platform_device *pdev)
{
	int rc;
	struct imdma_device *device_data;
	struct device *dev = &pdev->dev;

	dev_dbg(&pdev->dev, "imdma_probe(...)");

	// Allocate and attach memory for device
	device_data = (struct imdma_device *)devm_kzalloc(dev, sizeof(struct imdma_device), GFP_KERNEL);
	if (!device_data)
	{
		dev_err(dev, "failed to allocate memory for device data\n");
		return -ENOMEM;
	}

	// Store pointer to device for use later
	dev_set_drvdata(dev, device_data);
	device_data->device = &pdev->dev;

	mutex_init(&device_data->usage_count_mutex);

	rc = imdma_parse_dt(device_data);
	if (rc)
	{
		return rc;
	}

	dma_set_mask_and_coherent(device_data->device, DMA_BIT_MASK(device_data->address_width));

	// Request DMA channel (uses device tree "dmas" and "dma-names" properties)
	device_data->dma_channel = dma_request_chan(device_data->device, device_data->dma_channel_name);
	if (IS_ERR(device_data->dma_channel))
	{
		rc = PTR_ERR(device_data->dma_channel);
		if (rc != -EPROBE_DEFER)
		{
			dev_err(device_data->device, "request for DMA channel \"%s\" failed; rc = %d\n",
			        device_data->dma_channel_name, rc);
		}
		device_data->dma_channel = 0;
		return rc;
	}

	// Clear buffer pointers
	device_data->buffer_virtual_address = 0;
	device_data->buffer_bus_address = 0;

	// Create a character device for the channel
	rc = imdma_char_dev_create(device_data);
	if (rc)
	{
		dev_err(device_data->device, "imdma_char_dev_create error; rc=%d\n", rc);
		goto char_device_fail;
	}

	return 0;

char_device_fail:
	dma_release_channel(device_data->dma_channel);

	return rc;
}

static int imdma_remove(struct platform_device *pdev)
{
	struct imdma_device *device_data = dev_get_drvdata(&pdev->dev);

	dev_dbg(&pdev->dev, "imdma_remove(...)");

	imdma_char_dev_destroy(device_data);

	if (device_data->dma_channel)
	{
		device_data->dma_channel->device->device_terminate_all(device_data->dma_channel);

		dma_release_channel(device_data->dma_channel);
	}

	return 0;
}

// Module operations

static int __init imdma_init(void)
{
	// Create device class
	s_device_class = class_create(THIS_MODULE, IMDMA_DRIVER_NAME);
	if (IS_ERR(s_device_class))
	{
		return PTR_ERR(s_device_class);
	}

	s_device_class->dev_groups = imdma_attr_groups;

	// Register as platform driver
	return platform_driver_register(&imdma_driver);
}

static void __exit imdma_exit(void)
{
	// Unregister as platform driver
	platform_driver_unregister(&imdma_driver);

	// Destroy the device class
	if (s_device_class)
	{
		class_destroy(s_device_class);
	}
}

// Internal helper functions

static int imdma_transfer_start(struct imdma_device *device_data, struct imdma_transfer_start_spec *spec)
{
	struct dma_async_tx_descriptor *chan_desc;
	struct dma_device *dma_device = device_data->dma_channel->device;
	int buffer_index = spec->buffer_index;
	struct scatterlist *sg_list;

	device_data->buffer_statuses[buffer_index].length_bytes = spec->length_bytes;

	// Initialize and populate the scatter-gather list (with one entry)
	// TODO: use sg_init_one instead?
	sg_list = &device_data->buffer_statuses[buffer_index].sg_list;
	sg_init_table(sg_list, 1);
	sg_dma_address(sg_list) = device_data->buffer_statuses[buffer_index].dma_handle;
	sg_dma_len(sg_list) = device_data->buffer_statuses[buffer_index].length_bytes;

	dev_dbg(device_data->char_dev_device, "start_transfer: buffer_index = %d, dma_handle = 0x%px, length = %u",
	        buffer_index, (void *)device_data->buffer_statuses[buffer_index].dma_handle,
	        device_data->buffer_statuses[buffer_index].length_bytes);

	// Prepare the SG for DMA
	chan_desc = dma_device->device_prep_slave_sg(device_data->dma_channel, sg_list, 1, device_data->direction,
	                                             DMA_CTRL_ACK | DMA_PREP_INTERRUPT, NULL);
	if (!chan_desc)
	{
		dev_err(device_data->char_dev_device, "device_prep_slave_sg error\n");
		return -1;
	}

	// Attach completion callback
	chan_desc->callback = imdma_transfer_complete_callback;
	chan_desc->callback_param = &device_data->buffer_statuses[buffer_index];

	// Initialize the completion
	init_completion(&device_data->buffer_statuses[buffer_index].cmp);

	// Submit the transfer (SG) to the DMA engine (this queues up the transfer)
	device_data->buffer_statuses[buffer_index].cookie = dmaengine_submit(chan_desc);

	// Check for failures
	if (dma_submit_error(device_data->buffer_statuses[buffer_index].cookie))
	{
		dev_err(device_data->char_dev_device, "Submit error\n");
		return -1;
	}

	// Start any pending transfers
	// Note: if a transfer is in progress, the next transfer will be started at the completion of the current transfer.
	dma_async_issue_pending(device_data->dma_channel);

	return 0;
}

static int imdma_transfer_finish(struct imdma_device *device_data, struct imdma_transfer_finish_spec *spec)
{
	unsigned long timeout_jiffies;
	unsigned long remaining_jiffies;
	enum dma_status status;
	struct imdma_buffer_status *buffer_status;

	if (spec->timeout_ms == 0)
	{
		spec->timeout_ms = device_data->default_timeout_ms;
	}

	timeout_jiffies = msecs_to_jiffies(spec->timeout_ms);

	buffer_status = &device_data->buffer_statuses[spec->buffer_index];

	dev_dbg(device_data->char_dev_device, "wait_for_transfer: buffer_index = %d, dma_handle = 0x%px, timeout_ms = %u",
	        spec->buffer_index, (void *)buffer_status->dma_handle, spec->timeout_ms);

	// Wait for the transaction to complete, or timeout, or get an error
	remaining_jiffies = wait_for_completion_killable_timeout(&buffer_status->cmp, timeout_jiffies);
	status = dma_async_is_tx_complete(device_data->dma_channel, buffer_status->cookie, NULL, NULL);

	if (status == DMA_COMPLETE)
	{
		return 0;
	}
	else if (status == DMA_IN_PROGRESS)
	{
		dev_err(device_data->char_dev_device, "DMA in progress, but timed out\n");
		return -ETIMEDOUT;
	}
	else
	{
		dev_err(device_data->char_dev_device, "DMA transfer error: %d\n", status);
		return -EIO;
	}

	return 0;
}

static void imdma_transfer_complete_callback(void *buffer_status)
{
	struct imdma_buffer_status *status;
	struct imdma_device *device_data;
	enum dma_status dma_status;

	status = (struct imdma_buffer_status *)buffer_status;
	device_data = status->device_data;

	dev_dbg(status->device_data->char_dev_device, "Transfer complete for buffer %d\n", status->buffer_index);

	spin_lock(&status->buffer_state_spinlock);
	if (status->buffer_state != IMDMA_BUFFER_IN_PROGRESS)
	{
		dev_emerg(status->device_data->char_dev_device,
		          "Got completion callback for buffer (%d) that isn't in progress (%d)\n", status->buffer_index,
		          status->buffer_state);
	}
	spin_unlock(&status->buffer_state_spinlock);

	dma_status = dma_async_is_tx_complete(device_data->dma_channel, status->cookie, NULL, NULL);
	if (dma_status != DMA_COMPLETE)
	{
		dev_err(device_data->char_dev_device, "DMA transfer error: %d\n", dma_status);
	}

	status->buffer_state = IMDMA_BUFFER_DONE;

	complete(&status->cmp); // signal transaction completion
}

static void imdma_buffer_status_init(struct imdma_device *device_data, struct imdma_buffer_status *status,
                                     unsigned int buffer_index)
{
	spin_lock_init(&status->buffer_state_spinlock);
	status->buffer_state = IMDMA_BUFFER_FREE;
	status->buffer_index = buffer_index;
	status->buffer_offset = buffer_index * device_data->buffer_size_bytes;
	status->dma_handle = device_data->buffer_bus_address + status->buffer_offset;
	status->device_data = device_data;
}

static int imdma_buffer_alloc(struct imdma_device *device_data)
{
	int rc;
	int i;

	// Allocate DMA coherent memory that will be shared with user space
	device_data->buffer_virtual_address =
	    (unsigned char *)dmam_alloc_coherent(device_data->device,                                        // dev
	                                         device_data->buffer_size_bytes * device_data->buffer_count, // size
	                                         &device_data->buffer_bus_address, // dma_handle (out)
	                                         GFP_KERNEL);                      // flags

	if (!device_data->buffer_virtual_address)
	{
		dev_err(device_data->device, "DMA allocation error\n");
		return -ENOMEM;
	}

	dev_dbg(device_data->device, "alloc DMA memory; VAddr: %px, BAddr: %px, size: %u\n",
	        device_data->buffer_virtual_address, (void *)device_data->buffer_bus_address,
	        device_data->buffer_size_bytes * device_data->buffer_count);

	device_data->buffer_statuses =
	    devm_kzalloc(device_data->device, sizeof(struct imdma_buffer_status) * device_data->buffer_count, GFP_KERNEL);

	rc = 0;
	if (!device_data->buffer_statuses)
	{
		dev_err(device_data->device, "Buffer status allocation error\n");
		rc = -ENOMEM;
		goto buffer_alloc_fail;
	}

	for (i = 0; i < device_data->buffer_count; i++)
	{
		imdma_buffer_status_init(device_data, &device_data->buffer_statuses[i], i);
	}

	return 0;

buffer_alloc_fail:
	imdma_buffer_free(device_data);

	return rc;
}

static void imdma_buffer_free(struct imdma_device *device_data)
{
	if (device_data->buffer_virtual_address)
	{
		dev_dbg(device_data->device, "free DMA memory; VAddr: %px, BAddr: %px\n", device_data->buffer_virtual_address,
		        (void *)device_data->buffer_bus_address);

		dmam_free_coherent(device_data->device, device_data->buffer_size_bytes * device_data->buffer_count,
		                   device_data->buffer_virtual_address, device_data->buffer_bus_address);

		device_data->buffer_virtual_address = 0;
		device_data->buffer_bus_address = 0;
	}

	if (device_data->buffer_statuses)
	{
		devm_kfree(device_data->device, device_data->buffer_statuses);
		device_data->buffer_statuses = 0;
	}
}

static int imdma_parse_dt(struct imdma_device *device_data)
{
	int rc;
	int num_channels;
	unsigned int direction;

	// Query device tree for DMA channel names
	num_channels = device_property_read_string_array(device_data->device, "dma-names", NULL, 0);
	if (num_channels != 1)
	{
		dev_err(device_data->device, "dma-names property must have one and only one entry\n");
		return -ENODEV; // TODO: decide correct error code
	}

	// Read the DMA channel name into array
	rc = device_property_read_string_array(device_data->device, "dma-names", &device_data->dma_channel_name,
	                                       num_channels);
	if (rc < 0)
	{
		return rc;
	}

	// Read the name of the device
	rc = device_property_read_string(device_data->device, "imsar,name", &device_data->device_name);
	if (rc < 0)
	{
		dev_err(device_data->device, "missing or invalid imsar,name property\n");
		return rc;
	}

	// Read the direction
	rc = device_property_read_u32_array(device_data->device, "imsar,direction", &direction, 1);
	if (rc)
	{
		dev_err(device_data->device, "missing or invalid imsar,direction property\n");
		return rc;
	}
	device_data->direction = direction;

	// Read the desired buffer count
	rc = device_property_read_u32_array(device_data->device, "imsar,buffer-count", &device_data->buffer_count, 1);
	if (rc)
	{
		dev_err(device_data->device, "missing or invalid imsar,buffer-count property\n");
		return rc;
	}

	// Read the desired buffer count
	rc = device_property_read_u32_array(device_data->device, "imsar,buffer-size-bytes", &device_data->buffer_size_bytes,
	                                    1);
	if (rc)
	{
		dev_err(device_data->device, "missing or invalid imsar,buffer-count property\n");
		return rc;
	}

	// Read the address width
	rc = device_property_read_u32_array(device_data->device, "imsar,address-width", &device_data->address_width, 1);
	if (rc)
	{
		device_data->address_width = 32; // 32 bits
	}

	// Read the default timeout (ms)
	rc = device_property_read_u32_array(device_data->device, "imsar,default-timeout-ms",
	                                    &device_data->default_timeout_ms, 1);
	if (rc)
	{
		device_data->default_timeout_ms = 1000; // 1 second
	}

	return 0;
}

static int imdma_char_dev_create(struct imdma_device *device_data)
{
	int rc;

	// Allocate a character device region
	rc = alloc_chrdev_region(&device_data->char_dev_node, 0, 1, IMDMA_DRIVER_NAME);
	if (rc)
	{
		dev_err(device_data->device, "unable to get a char device number\n");
		return rc;
	}

	// Initialize the character device data structure
	cdev_init(&device_data->char_dev, &imdma_file_ops);
	device_data->char_dev.owner = THIS_MODULE;

	// Add the character device
	rc = cdev_add(&device_data->char_dev, device_data->char_dev_node, 1);
	if (rc)
	{
		dev_err(device_data->device, "unable to add char device\n");
		goto char_dev_fail;
	}

	// Create the device node in /dev
	device_data->char_dev_device = device_create(s_device_class, device_data->device, device_data->char_dev_node, NULL,
	                                             "imdma_%s", device_data->device_name);
	dev_set_drvdata(device_data->char_dev_device, device_data);

	if (IS_ERR(device_data->char_dev_device))
	{
		rc = -ENOMEM;
		dev_err(device_data->device, "unable to create the device\n");
		goto device_create_fail;
	}

	return 0;

device_create_fail:
	cdev_del(&device_data->char_dev);

char_dev_fail:
	unregister_chrdev_region(device_data->char_dev_node, 1);

	return rc;
}

static void imdma_char_dev_destroy(struct imdma_device *device_data)
{
	dev_dbg(device_data->device, "imdma_char_dev_destroy(...)");

	if (!device_data->char_dev_device)
	{
		return;
	}

	device_destroy(s_device_class, device_data->char_dev_node);

	cdev_del(&device_data->char_dev);

	unregister_chrdev_region(device_data->char_dev_node, 1);
}

ssize_t imdma_name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct imdma_device *device_data = dev_get_drvdata(dev);
	if (device_data->device_name)
	{
		return snprintf(buf, PAGE_SIZE, "%s\n", device_data->device_name);
	}
	else
	{
		return 0;
	}
}