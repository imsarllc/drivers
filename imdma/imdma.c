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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

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

#define DRIVER_NAME "imdma"

MODULE_AUTHOR("IMSAR, LLC.");
MODULE_DESCRIPTION("IMSAR DMA driver");
MODULE_LICENSE("GPL v2");

// ------------------------------------------------------------------
// Data structure definitions
// ------------------------------------------------------------------

enum imdma_buffer_state
{
	IMDMA_BUFFER_FREE,
	IMDMA_BUFFER_BUSY
};

struct imdma_buffer_status
{
	// read/write must be protected by mutex
	enum imdma_buffer_state status;
	struct mutex mutex;

	unsigned int length;
	struct completion cmp;
	dma_cookie_t cookie;
	dma_addr_t dma_handle;
	struct scatterlist sg_list;
};

struct imdma_device
{
	// DT properties
	const char *device_name;               // name
	const char *dma_channel_name;          // dma-names
	unsigned int buffer_count;             // imsar,buffer-count
	unsigned int buffer_size;              // imsar,buffer-size
	enum dma_transfer_direction direction; // imsar,direction

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
static long imdma_ioctl_transfer_start(struct imdma_device *device_data, unsigned long arg);
static long imdma_ioctl_transfer_finish(struct imdma_device *device_data, unsigned long arg);
static long imdma_ioctl_buffer_get_spec(struct imdma_device *device_data, unsigned long arg);

// Platform device operations
static int imdma_probe(struct platform_device *pdev);
static int imdma_remove(struct platform_device *pdev);

// Module operations
static int __init imdma_init(void);
static void __exit imdma_exit(void);

// Internal helper functions
static int imdma_start_transfer(struct imdma_device *device_data, struct imdma_transfer_spec *transfer_spec);
static void imdma_wait_for_transfer(struct imdma_device *device_data, struct imdma_transfer_spec *transfer_spec);
static void imdma_transfer_complete_callback(void *completion);
static int imdma_buffer_alloc(struct imdma_device *device_data);
static void imdma_buffer_free(struct imdma_device *device_data);
static int imdma_parse_dt(struct imdma_device *device_data);
static int imdma_char_dev_create(struct imdma_device *device_data);
static void imdma_char_dev_destroy(struct imdma_device *device_data);

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

static const struct of_device_id imdma_of_ids[] = {{.compatible = "imsar,dma-channel"}, {}};

static struct platform_driver imdma_driver = {
    .driver =
        {
            .name = "imdma",
            .owner = THIS_MODULE,
            .of_match_table = imdma_of_ids,
        },
    .probe = imdma_probe,
    .remove = imdma_remove,
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

	// Increment the usage count
	device_data->usage_count++;

	mutex_unlock(&device_data->usage_count_mutex);

	return rc;
}

static int imdma_release(struct inode *ino, struct file *file)
{
	int rc;
	struct imdma_device *device_data = (struct imdma_device *)file->private_data;

	rc = mutex_lock_interruptible(&device_data->usage_count_mutex);

	// Decrement usage count
	device_data->usage_count--;

	// If there are no more users, free the buffers
	if (device_data->usage_count == 0)
	{
		// TODO: ensure that all transactions have completed
		// dma_device->device_terminate_all(pchannel_p->channel_p);
		imdma_buffer_free(device_data);
	}

	mutex_unlock(&device_data->usage_count_mutex);

	return 0;
}

static int imdma_mmap(struct file *file_p, struct vm_area_struct *vma)
{
	// TODO: validate requested range?????
	struct imdma_device *device_data = (struct imdma_device *)file_p->private_data;
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
	switch (cmd)
	{
	case IMDMA_TRANSFER_START:
		return imdma_ioctl_transfer_start(device_data, arg);
	case IMDMA_TRANSFER_FINISH:
		return imdma_ioctl_transfer_finish(device_data, arg);
	case IMDMA_BUFFER_GET_SPEC:
		return imdma_ioctl_buffer_get_spec(device_data, arg);
	default:
		return -EINVAL;
	}
}

// ioctl implementations

static long imdma_ioctl_transfer_start(struct imdma_device *device_data, unsigned long arg)
{
	int rc;
	struct imdma_transfer_spec transfer_spec;

	if (copy_from_user(&transfer_spec, (int *)arg, sizeof(transfer_spec)))
	{
		dev_warn(device_data->device, "copy_from_user failed");
		return -EINVAL;
	}

	if (transfer_spec.buffer_index >= device_data->buffer_count)
	{
		dev_warn(device_data->device, "buffer index out of bounds");
		return -EINVAL;
	}

	if (transfer_spec.length >= device_data->buffer_size)
	{
		dev_warn(device_data->device, "length is greater than buffer size");
		return -EINVAL;
	}

	rc = mutex_lock_interruptible(&device_data->buffer_statuses[transfer_spec.buffer_index].mutex);
	if (rc)
	{
		dev_dbg(device_data->device, "buffer %d status check interrupted", transfer_spec.buffer_index);
		return -EINTR;
	}

	if (device_data->buffer_statuses[transfer_spec.buffer_index].status != IMDMA_BUFFER_FREE)
	{
		mutex_unlock(&device_data->buffer_statuses[transfer_spec.buffer_index].mutex);
		dev_warn(device_data->device, "buffer %d is busy", transfer_spec.buffer_index);
		return -EBUSY;
	}

	device_data->buffer_statuses[transfer_spec.buffer_index].status = IMDMA_BUFFER_BUSY;

	rc = imdma_start_transfer(device_data, &transfer_spec);
	if (rc)
	{
		device_data->buffer_statuses[transfer_spec.buffer_index].status = IMDMA_BUFFER_FREE;
		mutex_unlock(&device_data->buffer_statuses[transfer_spec.buffer_index].mutex);
		dev_warn(device_data->device, "buffer %d failed to start transfer", transfer_spec.buffer_index);
		return rc;
	}

	mutex_unlock(&device_data->buffer_statuses[transfer_spec.buffer_index].mutex);

	return 0;
}

static long imdma_ioctl_transfer_finish(struct imdma_device *device_data, unsigned long arg)
{
	int rc;
	struct imdma_transfer_spec transfer_spec;

	if (copy_from_user(&transfer_spec, (struct imdma_transfer_spec *)arg, sizeof(transfer_spec)))
	{
		return -EINVAL;
	}

	if (transfer_spec.buffer_index >= device_data->buffer_count)
	{
		dev_warn(device_data->device, "buffer index out of bounds");
		return -EINVAL;
	}

	rc = mutex_lock_interruptible(&device_data->buffer_statuses[transfer_spec.buffer_index].mutex);
	if (rc)
	{
		dev_dbg(device_data->device, "buffer %d status check interrupted", transfer_spec.buffer_index);
		return -EINTR;
	}

	if (device_data->buffer_statuses[transfer_spec.buffer_index].status != IMDMA_BUFFER_BUSY)
	{
		mutex_unlock(&device_data->buffer_statuses[transfer_spec.buffer_index].mutex);
		dev_warn(device_data->device, "buffer %d not in progress", transfer_spec.buffer_index);
		return -EINVAL;
	}

	imdma_wait_for_transfer(device_data, &transfer_spec);

	device_data->buffer_statuses[transfer_spec.buffer_index].status = IMDMA_BUFFER_FREE;

	mutex_unlock(&device_data->buffer_statuses[transfer_spec.buffer_index].mutex);

	if (copy_to_user((struct imdma_transfer_spec *)arg, &transfer_spec, sizeof(transfer_spec)))
	{
		return -EINVAL;
	}

	return 0;
}

static long imdma_ioctl_buffer_get_spec(struct imdma_device *device_data, unsigned long arg)
{
	struct imdma_buffer_spec buffer_spec;

	buffer_spec.count = device_data->buffer_count;
	buffer_spec.size = device_data->buffer_size;

	if (copy_to_user((struct imdma_buffer_spec *)arg, &buffer_spec, sizeof(buffer_spec)))
	{
		return -EINVAL;
	}

	return 0;
}

// Platform device operations

static int imdma_probe(struct platform_device *pdev)
{
	int rc;
	struct imdma_device *device_data;
	struct device *dev = &pdev->dev;

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

	// Request DMA channel (uses device tree "dmas" and "dma-names" properties)
	device_data->dma_channel = dma_request_chan(device_data->device, device_data->dma_channel_name);
	if (IS_ERR(device_data->dma_channel))
	{
		rc = PTR_ERR(device_data->dma_channel);
		dev_err(device_data->device, "request for DMA channel \"%s\" failed; rc = %d\n", device_data->dma_channel_name,
		        rc);
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
	s_device_class = class_create(THIS_MODULE, DRIVER_NAME);
	if (IS_ERR(s_device_class))
	{
		return PTR_ERR(s_device_class);
	}

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

static int imdma_start_transfer(struct imdma_device *device_data, struct imdma_transfer_spec *transfer_spec)
{
	enum dma_ctrl_flags flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
	struct dma_async_tx_descriptor *chan_desc;
	struct dma_device *dma_device = device_data->dma_channel->device;
	int buffer_index = transfer_spec->buffer_index;
	struct scatterlist *sg_list;

	device_data->buffer_statuses[buffer_index].length = transfer_spec->length;

	// Initialize and populate the scatter-gather list (with one entry)
	// TODO: use sg_init_one instead?
	sg_list = &device_data->buffer_statuses[buffer_index].sg_list;
	sg_init_table(sg_list, 1);
	sg_dma_address(sg_list) = device_data->buffer_statuses[buffer_index].dma_handle;
	sg_dma_len(sg_list) = device_data->buffer_statuses[buffer_index].length;

	dev_info(device_data->char_dev_device, "start_transfer: buffer_index = %d, dma_handle = 0x%px, length = %u",
	         buffer_index, (void *)device_data->buffer_statuses[buffer_index].dma_handle,
	         device_data->buffer_statuses[buffer_index].length);

	// Prepare the SG for DMA
	chan_desc =
	    dma_device->device_prep_slave_sg(device_data->dma_channel, sg_list, 1, device_data->direction, flags, NULL);
	if (!chan_desc)
	{
		dev_err(device_data->char_dev_device, "device_prep_slave_sg error\n");
		return -1;
	}

	// Attach completion callback
	chan_desc->callback = imdma_transfer_complete_callback;
	chan_desc->callback_param = &device_data->buffer_statuses[buffer_index].cmp;

	// Initialize the completion
	init_completion(&device_data->buffer_statuses[buffer_index].cmp);

	// Attempt to submit the SG to the DMA engine
	device_data->buffer_statuses[buffer_index].cookie = dmaengine_submit(chan_desc);

	// Check for failures
	if (dma_submit_error(device_data->buffer_statuses[buffer_index].cookie))
	{
		dev_err(device_data->char_dev_device, "Submit error\n");
		return -1;
	}

	// Finally, issue the DMA
	dma_async_issue_pending(device_data->dma_channel);

	return 0;
}

static void imdma_wait_for_transfer(struct imdma_device *device_data, struct imdma_transfer_spec *transfer_spec)
{
	unsigned long timeout = msecs_to_jiffies(transfer_spec->timeout_ms);
	enum dma_status status;
	int buffer_index = transfer_spec->buffer_index;


	// Wait for the transaction to complete, or timeout, or get an error
	timeout = wait_for_completion_timeout(&device_data->buffer_statuses[buffer_index].cmp, timeout);
	status = dma_async_is_tx_complete(device_data->dma_channel, device_data->buffer_statuses[buffer_index].cookie, NULL,
	                                  NULL);

	if (timeout == 0)
	{
		transfer_spec->status = IMDMA_STATUS_TIMEOUT;
		dev_err(device_data->char_dev_device, "DMA timed out\n");
		return;
	}

	if (status != DMA_COMPLETE)
	{
		transfer_spec->status = IMDMA_STATUS_ERROR;
		dev_err(device_data->char_dev_device, "DMA returned completion callback status of: %s\n",
		        status == DMA_ERROR ? "error" : "in progress");
		return;
	}

	transfer_spec->length = device_data->buffer_statuses[buffer_index].length;
	transfer_spec->offset = buffer_index * device_data->buffer_size;
	transfer_spec->status = IMDMA_STATUS_COMPLETE;
}

static void imdma_transfer_complete_callback(void *completion)
{
	complete(completion); // signal transaction completion
}

static int imdma_buffer_alloc(struct imdma_device *device_data)
{
	int rc;
	int i;

	// Allocate DMA coherent memory that will be shared with user space
	device_data->buffer_virtual_address =
	    (unsigned char *)dmam_alloc_coherent(device_data->device,                                  // dev
	                                         device_data->buffer_size * device_data->buffer_count, // size
	                                         &device_data->buffer_bus_address,                     // dma_handle (out)
	                                         GFP_KERNEL);                                          // flags

	if (!device_data->buffer_virtual_address)
	{
		dev_err(device_data->device, "DMA allocation error\n");
		return -ENOMEM;
	}

	dev_info(device_data->device, "alloc DMA memory; VAddr: %px, BAddr: %px, size: %u\n",
	         device_data->buffer_virtual_address, (void *)device_data->buffer_bus_address,
	         device_data->buffer_size * device_data->buffer_count);

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
		mutex_init(&device_data->buffer_statuses[i].mutex);
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

		dev_info(device_data->device, "free DMA memory; VAddr: %px, BAddr: %px\n", device_data->buffer_virtual_address,
		         (void *)device_data->buffer_bus_address);
		dmam_free_coherent(device_data->device, device_data->buffer_size * device_data->buffer_count,
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
	rc = device_property_read_u32_array(device_data->device, "imsar,buffer-size", &device_data->buffer_size, 1);
	if (rc)
	{
		dev_err(device_data->device, "missing or invalid imsar,buffer-count property\n");
		return rc;
	}

	return 0;
}

static int imdma_char_dev_create(struct imdma_device *device_data)
{
	int rc;
	char device_name_full[32] = "imdma_";

	// Allocate a character device region
	rc = alloc_chrdev_region(&device_data->char_dev_node, 0, 1, DRIVER_NAME);
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
	strcat(device_name_full, device_data->device_name);
	device_data->char_dev_device =
	    device_create(s_device_class, NULL, device_data->char_dev_node, NULL, device_name_full);

	if (IS_ERR(device_data->char_dev_device))
	{
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
	if (!device_data->char_dev_device)
	{
		return;
	}

	device_destroy(s_device_class, device_data->char_dev_node);
	cdev_del(&device_data->char_dev);
	unregister_chrdev_region(device_data->char_dev_node, 1);
}
