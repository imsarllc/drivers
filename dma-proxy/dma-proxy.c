#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/workqueue.h>

#include "dma-proxy.h"

#define DRIVER_NAME "imsar_dma_proxy"
#define ERROR -1

static struct class *s_class_p = NULL;

// The following data structures represent a single channel of DMA, transmit or receive in the case
// when using AXI DMA.  It contains all the data to be maintained for the channel.
struct proxy_buffer_desc
{
	struct completion cmp;
	dma_cookie_t cookie;
	dma_addr_t dma_handle;
	struct scatterlist sg_list;
};

struct dma_proxy_channel
{
	struct channel_buffer *buffer_table_p; // user/kernel space interface
	dma_addr_t buffer_phys_addr;

	struct device *proxy_char_dev_p; // character device support
	struct device *dma_dev_p;
	dev_t char_dev_node;
	struct cdev cdev;
	struct class *class_p;

	struct proxy_buffer_desc buf_descr_table[BUFFER_COUNT];

	struct dma_chan *channel_p; // dma support
	u32 direction;              // DMA_MEM_TO_DEV or DMA_DEV_TO_MEM
	int buf_descr_index;
};

struct dma_proxy
{
	int channel_count;
	struct dma_proxy_channel *channels;
	char **names;
	struct work_struct work;
};

static int total_count = 0;

// Handle a callback and indicate the DMA transfer is complete to another
// thread of control
static void sync_callback(void *completion)
{
	// Indicate the DMA transaction completed to allow the other
	// thread of control to finish processing
	complete(completion);
}

// Prepare a DMA buffer to be used in a DMA transaction, submit it to the DMA engine
// to be queued and return a cookie that can be used to track that status of the
// transaction
static void start_transfer(struct dma_proxy_channel *pchannel_p)
{
	enum dma_ctrl_flags flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
	struct dma_async_tx_descriptor *chan_desc;
	struct dma_device *dma_device = pchannel_p->channel_p->device;
	int buf_descr_index = pchannel_p->buf_descr_index;

	// A single entry scatter gather list is used as it's not clear how to do it with a simpler method.
	// Get a descriptor for the transfer ready to submit
	struct scatterlist *scatter_list_p = &pchannel_p->buf_descr_table[buf_descr_index].sg_list;
	sg_init_table(scatter_list_p, 1);
	sg_dma_address(scatter_list_p) = pchannel_p->buf_descr_table[buf_descr_index].dma_handle;
	sg_dma_len(scatter_list_p) = pchannel_p->buffer_table_p[buf_descr_index].length;

	chan_desc =
	    dma_device->device_prep_slave_sg(pchannel_p->channel_p, scatter_list_p, 1, pchannel_p->direction, flags, NULL);

	if (!chan_desc)
	{
		printk(KERN_ERR "dmaengine_prep*() error\n");
	}
	else
	{
		chan_desc->callback = sync_callback;
		chan_desc->callback_param = &pchannel_p->buf_descr_table[buf_descr_index].cmp;

		// Initialize the completion for the transfer and before using it
		// then submit the transaction to the DMA engine so that it's queued
		// up to be processed later and get a cookie to track it's status
		init_completion(&pchannel_p->buf_descr_table[buf_descr_index].cmp);

		pchannel_p->buf_descr_table[buf_descr_index].cookie = dmaengine_submit(chan_desc);
		if (dma_submit_error(pchannel_p->buf_descr_table[buf_descr_index].cookie))
		{
			printk("Submit error\n");
			return;
		}

		// Start the DMA transaction which was previously queued up in the DMA engine
		dma_async_issue_pending(pchannel_p->channel_p);
	}
}

// Wait for a DMA transfer that was previously submitted to the DMA engine
static void wait_for_transfer(struct dma_proxy_channel *pchannel_p)
{
	unsigned long timeout = msecs_to_jiffies(3000);
	enum dma_status status;
	int buf_descr_index = pchannel_p->buf_descr_index;

	pchannel_p->buffer_table_p[buf_descr_index].status = PROXY_BUSY;

	// Wait for the transaction to complete, or timeout, or get an error
	timeout = wait_for_completion_timeout(&pchannel_p->buf_descr_table[buf_descr_index].cmp, timeout);
	status = dma_async_is_tx_complete(pchannel_p->channel_p, pchannel_p->buf_descr_table[buf_descr_index].cookie, NULL,
	                                  NULL);

	if (timeout == 0)
	{
		pchannel_p->buffer_table_p[buf_descr_index].status = PROXY_TIMEOUT;
		printk(KERN_ERR "DMA timed out\n");
	}
	else if (status != DMA_COMPLETE)
	{
		pchannel_p->buffer_table_p[buf_descr_index].status = PROXY_ERROR;
		printk(KERN_ERR "DMA returned completion callback status of: %s\n",
		       status == DMA_ERROR ? "error" : "in progress");
	}
	else
	{
		pchannel_p->buffer_table_p[buf_descr_index].status = PROXY_NO_ERROR;
	}
}

// Map the memory for the channel interface into user space such that user space can
// access it using coherent memory which will be non-cached for s/w coherent systems
// such as Zynq 7K or the current default for Zynq MPSOC. MPSOC can be h/w coherent
// when set up and then the memory will be cached.
static int dma_proxy_mmap(struct file *file_p, struct vm_area_struct *vma)
{
	struct dma_proxy_channel *pchannel_p = (struct dma_proxy_channel *)file_p->private_data;

	return dma_mmap_coherent(pchannel_p->dma_dev_p,        // dev
	                         vma,                          // vma
	                         pchannel_p->buffer_table_p,   // cpu_addr
	                         pchannel_p->buffer_phys_addr, // handle
	                         vma->vm_end - vma->vm_start   // size
	);
}

// Open the device file and set up the data pointer to the proxy channel data for the
// proxy channel such that the ioctl function can access the data structure later.
static int dma_proxy_open(struct inode *ino, struct file *file)
{
	file->private_data = container_of(ino->i_cdev, struct dma_proxy_channel, cdev);

	return 0;
}

// Close the file and there's nothing to do for it
static int dma_proxy_release(struct inode *ino, struct file *file)
{
#if 0
    struct dma_proxy_channel *pchannel_p = (struct dma_proxy_channel *)file->private_data;
    struct dma_device *dma_device = pchannel_p->channel_p->device;

    // Stop all the activity when the channel is closed assuming this
    // may help if the application is aborted without normal closure
    // This is not working and causes an issue that may need investigation in the
    // DMA driver at the lower level.
    dma_device->device_terminate_all(pchannel_p->channel_p);
#endif
	return 0;
}

// Perform I/O control to perform a DMA transfer using the input as an index
// into the buffer descriptor table such that the application is in control of
// which buffer to use for the transfer.The BD in this case is only a s/w
// structure for the proxy driver, not related to the hw BD of the DMA.
static long dma_proxy_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct dma_proxy_channel *pchannel_p = (struct dma_proxy_channel *)file->private_data;

	// Get the buffer_descr index from the input argument as all commands require it
	if (copy_from_user(&pchannel_p->buf_descr_index, (int *)arg, sizeof(pchannel_p->buf_descr_index)))
	{
		return -EINVAL;
	}

	// Perform the DMA transfer on the specified channel blocking til it completes
	switch (cmd)
	{
	case START_XFER:
		start_transfer(pchannel_p);
		break;
	case FINISH_XFER:
		wait_for_transfer(pchannel_p);
		break;
	case XFER:
		start_transfer(pchannel_p);
		wait_for_transfer(pchannel_p);
		break;
	}

	return 0;
}

static struct file_operations dm_fops = {
    .owner = THIS_MODULE,              //
    .open = dma_proxy_open,            //
    .release = dma_proxy_release,      //
    .unlocked_ioctl = dma_proxy_ioctl, //
    .mmap = dma_proxy_mmap             //
};


// Initialize the driver to be a character device such that is responds to
// file operations.
static int cdevice_init(struct dma_proxy_channel *pchannel_p, char *name)
{
	int rc;
	char device_name[32] = "dma_proxy";

	// Allocate a character device from the kernel for this driver.
	rc = alloc_chrdev_region(&pchannel_p->char_dev_node, 0, 1, DRIVER_NAME);

	if (rc)
	{
		dev_err(pchannel_p->dma_dev_p, "unable to get a char device number\n");
		return rc;
	}

	// Initialize the device data structure before registering the character
	// device with the kernel.
	cdev_init(&pchannel_p->cdev, &dm_fops);
	pchannel_p->cdev.owner = THIS_MODULE;
	rc = cdev_add(&pchannel_p->cdev, pchannel_p->char_dev_node, 1);

	if (rc)
	{
		dev_err(pchannel_p->dma_dev_p, "unable to add char device\n");
		goto init_error1;
	}

	pchannel_p->class_p = s_class_p;

	// Create the device node in /dev so the device is accessible
	// as a character device
	strcat(device_name, name);
	pchannel_p->proxy_char_dev_p = device_create(pchannel_p->class_p, NULL, pchannel_p->char_dev_node, NULL, name);

	if (IS_ERR(pchannel_p->proxy_char_dev_p))
	{
		dev_err(pchannel_p->dma_dev_p, "unable to create the device\n");
		goto init_error2;
	}

	return 0;

init_error2:
	cdev_del(&pchannel_p->cdev);

init_error1:
	unregister_chrdev_region(pchannel_p->char_dev_node, 1);
	return rc;
}

// Exit the character device by freeing up the resources that it created and
// disconnecting itself from the kernel.
static void cdevice_exit(struct dma_proxy_channel *pchannel_p)
{
	// Take everything down in the reverse order
	// from how it was created for the char device
	if (pchannel_p->proxy_char_dev_p)
	{
		device_destroy(pchannel_p->class_p, pchannel_p->char_dev_node);

		// If this is the last channel then get rid of the /sys/class/dma_proxy
		if (total_count == 1)
		{
			class_destroy(pchannel_p->class_p);
		}

		cdev_del(&pchannel_p->cdev);
		unregister_chrdev_region(pchannel_p->char_dev_node, 1);
	}
}

// Create a DMA channel by getting a DMA channel from the DMA Engine and then setting
// up the channel as a character device to allow user space control.
static int create_channel(struct platform_device *pdev, struct dma_proxy_channel *pchannel_p, char *name, u32 direction)
{
	int rc, buffer_descr;

	// Request the DMA channel from the DMA engine and then use the device from
	// the channel for the proxy channel also.
	pchannel_p->dma_dev_p = &pdev->dev;
	pchannel_p->channel_p = dma_request_chan(pchannel_p->dma_dev_p, name);
	if (!pchannel_p->channel_p)
	{
		dev_err(pchannel_p->dma_dev_p, "DMA channel request error\n");
		return ERROR;
	}

	// Initialize the character device for the dma proxy channel
	rc = cdevice_init(pchannel_p, name);
	if (rc)
	{
		dev_err(pchannel_p->dma_dev_p, "cdevice_init error=%d\n", rc);
		goto cdevice_fail;
	}

	pchannel_p->direction = direction;

	// Allocate DMA memory that will be shared/mapped by user space, allocating
	// a set of buffers for the channel with user space specifying which buffer
	// to use for a tranfer..
	pchannel_p->buffer_table_p =
	    (struct channel_buffer *)dmam_alloc_coherent(pchannel_p->dma_dev_p,                        // dev
	                                                 sizeof(struct channel_buffer) * BUFFER_COUNT, // size
	                                                 &pchannel_p->buffer_phys_addr,                // dma_handle (out)
	                                                 GFP_KERNEL);                                  // flags

	if (pchannel_p->buffer_table_p)
	{
		dev_info(pchannel_p->dma_dev_p, "Allocated memory, virtual address: %px physical address: %px\n",
		         pchannel_p->buffer_table_p, (void *)pchannel_p->buffer_phys_addr);
	}
	else
	{
		dev_err(pchannel_p->dma_dev_p, "DMA allocation error\n");
		rc = ERROR;
		goto alloc_fail;
	}

	// Initialize each entry in the buffer descriptor table such that the physical address
	// address of each buffer is ready to use later.
	for (buffer_descr = 0; buffer_descr < BUFFER_COUNT; buffer_descr++)
	{
		pchannel_p->buf_descr_table[buffer_descr].dma_handle =
		    (dma_addr_t)(pchannel_p->buffer_phys_addr + (sizeof(struct channel_buffer) * buffer_descr) +
		                 offsetof(struct channel_buffer, buffer));
	}

	// The buffer descriptor index into the channel buffers should be specified in each
	// ioctl but we will initialize it to be safe.
	pchannel_p->buf_descr_index = 0;

	return 0;


alloc_fail:
	cdevice_exit(pchannel_p);

cdevice_fail:
	dma_release_channel(pchannel_p->channel_p);

	return rc;
}

// Initialize the dma proxy device driver module.
static int dma_proxy_probe(struct platform_device *pdev)
{
	int rc, i;
	struct dma_proxy *lp;
	struct device *dev = &pdev->dev;

	dev_info(dev, "probing\n");

	lp = (struct dma_proxy *)devm_kmalloc(dev, sizeof(struct dma_proxy), GFP_KERNEL);
	if (!lp)
	{
		dev_err(dev, "cound not allocate proxy device\n");
		return -ENOMEM;
	}
	dev_set_drvdata(dev, lp);

	// Figure out how many channels there are from the device tree based
	// on the number of strings in the dma-names property
	lp->channel_count = device_property_read_string_array(dev, "dma-names", NULL, 0);
	if (lp->channel_count <= 0)
	{
		return 0;
	}

	dev_info(dev, "channel count:  %d\n", lp->channel_count);

	// Allocate the memory for channel names and then get the names
	// from the device tree
	lp->names = devm_kmalloc_array(dev, lp->channel_count, sizeof(char *), GFP_KERNEL);
	if (!lp->names)
	{
		return -ENOMEM;
	}

	rc = device_property_read_string_array(dev, "dma-names", (const char **)lp->names, lp->channel_count);
	if (rc < 0)
	{
		return rc;
	}

	// Allocate the memory for the channels since the number is known.
	lp->channels = devm_kmalloc(dev, sizeof(struct dma_proxy_channel) * lp->channel_count, GFP_KERNEL);
	if (!lp->channels)
	{
		return -ENOMEM;
	}

	// Create the channels in the proxy. The direction does not matter
	// as the DMA channel has it inside it and uses it, other than this will not work
	// for cyclic mode.
	for (i = 0; i < lp->channel_count; i++)
	{
		dev_info(dev, "creating channel %s (%i)\n", lp->names[i], i);

		rc = create_channel(pdev, &lp->channels[i], lp->names[i], DMA_DEV_TO_MEM);

		if (rc)
		{
			dev_info(dev, "failed to create channel %s (%d), err=%d\n", lp->names[i], i, rc);
			goto create_channel_fail;
		}

		total_count++;
	}

	return 0;

create_channel_fail:
	for (i--; i >= 0; i--)
	{
		if (lp->channels[i].channel_p)
		{
			dma_release_channel(lp->channels[i].channel_p);
		}
	}

	return rc;
}

// Exit the dma proxy device driver module.
static int dma_proxy_remove(struct platform_device *pdev)
{
	int i;
	struct device *dev = &pdev->dev;
	struct dma_proxy *lp = dev_get_drvdata(dev);

	printk(KERN_INFO "dma_proxy module exited\n");

	// Take care of the char device infrastructure for each
	// channel except for the last channel. Handle the last
	// channel seperately.
	for (i = 0; i < lp->channel_count; i++)
	{
		if (lp->channels[i].proxy_char_dev_p)
		{
			cdevice_exit(&lp->channels[i]);
		}
		total_count--;
	}

	// Take care of the DMA channels and any buffers allocated
	// for the DMA transfers. The DMA buffers are using managed
	// memory such that it's automatically done.
	for (i = 0; i < lp->channel_count; i++)
	{
		if (lp->channels[i].channel_p)
		{
			lp->channels[i].channel_p->device->device_terminate_all(lp->channels[i].channel_p);
			dma_release_channel(lp->channels[i].channel_p);
		}
	}

	return 0;
}

static const struct of_device_id dma_proxy_of_ids[] = {{
                                                           .compatible = "xlnx,dma_proxy",
                                                       },
                                                       {}};

static struct platform_driver dma_proxy_driver = {
    .driver =
        {
            .name = "dma_proxy_driver",
            .owner = THIS_MODULE,
            .of_match_table = dma_proxy_of_ids,
        },
    .probe = dma_proxy_probe,
    .remove = dma_proxy_remove,
};

static int __init dma_proxy_init(void)
{
	s_class_p = class_create(THIS_MODULE, DRIVER_NAME);
	if (IS_ERR(s_class_p))
	{
		return PTR_ERR(s_class_p);
	}

	return platform_driver_register(&dma_proxy_driver);
}

static void __exit dma_proxy_exit(void)
{
	if (s_class_p)
	{
		class_destroy(s_class_p);
	}

	platform_driver_unregister(&dma_proxy_driver);
}

module_init(dma_proxy_init);
module_exit(dma_proxy_exit);

MODULE_AUTHOR("IMSAR, LLC.");
MODULE_DESCRIPTION("IMSAR DMA Proxy");
MODULE_LICENSE("GPL v2");
