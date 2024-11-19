#include "imsar_user_interrupt.h"

#include "version.h"
#include <asm-generic/errno.h>
#include <asm/io.h>
#include <asm/ioctls.h>
#include <linux/cdev.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/wait.h>

#define IMSAR_USER_INTERRUPT_DRIVER_NAME "imsar_user_interrupt"

MODULE_AUTHOR("IMSAR, LLC. Embedded Team <embedded@imsar.com>");
MODULE_DESCRIPTION("IMSAR User Space Interrupt Driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION(GIT_DESCRIBE);

// ------------------------------------------------------------------
// Data structure definitions
// ------------------------------------------------------------------

typedef struct
{
	// Device Tree properties
	const char *name;                // name of the interrupt
	unsigned int default_timeout_ms; // imsar,default-timeout-ms
	char interrupt_name[64];

	// Device
	struct platform_device *platform_dev;
	struct device *device;

	// Character device
	dev_t char_dev_node;
	struct cdev char_dev;
	struct device *char_dev_device;

	// Device state variables
	int irq;                       // request number
	struct mutex irq_change_mutex; // ensure only one user call can enable/disable interrupts simultaneously

	// File(s)
	spinlock_t consumers_spinlock;    // held when changing consuming_files
	struct list_head consuming_files; // points at imsar_user_interrupt_file_t entries
} imsar_user_interrupt_device_t;

typedef struct
{
	imsar_user_interrupt_device_t *interrupt_dev;
	unsigned int timeout_ms;
	wait_queue_head_t file_waitqueue;
	int interrupt_count;   // incremented when an interrupt occurs, cleared on a call to read()
	struct list_head list; // used to link pointers for consuming_files
} imsar_user_interrupt_file_t;


// ------------------------------------------------------------------
// Forward function declarations
// ------------------------------------------------------------------

// File operations
static int imsar_user_interrupt_open(struct inode *inode, struct file *file);
static int imsar_user_interrupt_release(struct inode *inode, struct file *file);
static ssize_t imsar_user_interrupt_write(struct file *file, const char __user *buf, size_t bytes, loff_t *ppos);
static ssize_t imsar_user_interrupt_read(struct file *file, char __user *buf, size_t bytes, loff_t *off);
static unsigned int imsar_user_interrupt_poll(struct file *file, poll_table *wait);
static long imsar_user_interrupt_ioctl(struct file *file, unsigned int request, unsigned long arg);

// Platform device operations
static int imsar_user_interrupt_probe(struct platform_device *platform_dev);
static int imsar_user_interrupt_remove(struct platform_device *platform_dev);

// Module operations
static int __init imsar_user_interrupt_init(void);
static void __exit imsar_user_interrupt_exit(void);

// Internal helper functions
static int imsar_user_interrupt_device_data_init(struct platform_device *platform_dev,
                                                 imsar_user_interrupt_device_t *device_data);
static int imsar_user_interrupt_char_dev_create(imsar_user_interrupt_device_t *device_data);
static void imsar_user_interrupt_char_dev_destroy(imsar_user_interrupt_device_t *device_data);
static int imsar_user_interrupt_attach_irq(imsar_user_interrupt_device_t *device_data);
static void imsar_user_interrupt_detach_irq(imsar_user_interrupt_device_t *device_data);
static irqreturn_t imsar_user_interrupt_handle_irq(int num, void *dev_id);

// Attributes
ssize_t imsar_user_interrupt_name_show(struct device *dev, struct device_attribute *attr, char *buf);

// ------------------------------------------------------------------
// Static variables
// ------------------------------------------------------------------

static struct class *s_device_class;

static struct file_operations imsar_user_interrupts_fops = {
    .owner = THIS_MODULE,
    .open = imsar_user_interrupt_open,
    .release = imsar_user_interrupt_release,
    .write = imsar_user_interrupt_write,
    .read = imsar_user_interrupt_read,
    .poll = imsar_user_interrupt_poll,
    .unlocked_ioctl = imsar_user_interrupt_ioctl,
};

static const struct of_device_id imsar_user_interrupt_match_ids[] = {
    {
        .compatible = "imsar,user-interrupt",
    },
    {/* end of list */},
};
MODULE_DEVICE_TABLE(of, imsar_user_interrupt_match_ids);

static struct platform_driver imsar_user_interrupt_driver = {
    .probe = imsar_user_interrupt_probe,
    .remove = imsar_user_interrupt_remove,
    .driver =
        {
            .name = IMSAR_USER_INTERRUPT_DRIVER_NAME,
            .owner = THIS_MODULE,
            .of_match_table = imsar_user_interrupt_match_ids,
        },
};

static DEVICE_ATTR(name, S_IRUGO, imsar_user_interrupt_name_show, NULL);

static struct attribute *imsar_user_interrupt_attrs[] = {
    &dev_attr_name.attr,
    NULL,
};
static struct attribute_group attr_imsar_user_interrupt_attr_group = {
    .attrs = imsar_user_interrupt_attrs,
};
static const struct attribute_group *imsar_user_interrupt_attr_groups[] = {
    &attr_imsar_user_interrupt_attr_group,
    NULL,
};

// ------------------------------------------------------------------
// Function definitions
// ------------------------------------------------------------------

static int imsar_user_interrupt_open(struct inode *inode, struct file *file)
{
	int rc;
	imsar_user_interrupt_device_t *device_data;
	imsar_user_interrupt_file_t *file_data;
	int list_was_empty;

	device_data = container_of(inode->i_cdev, imsar_user_interrupt_device_t, char_dev);

	file_data = kzalloc(sizeof(imsar_user_interrupt_file_t), GFP_KERNEL);
	if (file_data == NULL)
	{
		return -ENOMEM;
	}

	file->private_data = file_data;
	file_data->timeout_ms = device_data->default_timeout_ms;
	file_data->interrupt_dev = device_data;
	init_waitqueue_head(&file_data->file_waitqueue);
	INIT_LIST_HEAD(&file_data->list);
	file_data->interrupt_count = 0;

	rc = mutex_lock_interruptible(&device_data->irq_change_mutex);
	if (rc)
	{
		return -rc;
	}

	spin_lock(&device_data->consumers_spinlock);
	list_was_empty = list_empty(&device_data->consuming_files);
	list_add_tail(&file_data->list, &device_data->consuming_files);
	spin_unlock(&device_data->consumers_spinlock);

	// Attach IRQ on first user
	if (list_was_empty)
	{
		rc = imsar_user_interrupt_attach_irq(device_data);
		if (rc < 0)
		{
			dev_err(device_data->device, "attach_irq failed rc=%d\n", rc);
			goto handle_irq_error;
		}
	}

	mutex_unlock(&device_data->irq_change_mutex);

handle_irq_error:
	return rc;
}

static int imsar_user_interrupt_release(struct inode *inode, struct file *file)
{
	int rc;
	int list_is_empty;
	imsar_user_interrupt_file_t *file_data;
	imsar_user_interrupt_device_t *device_data;

	file_data = (imsar_user_interrupt_file_t *)file->private_data;
	device_data = file_data->interrupt_dev;

	rc = mutex_lock_interruptible(&device_data->irq_change_mutex);
	if (rc)
	{
		return -rc;
	}

	spin_lock(&device_data->consumers_spinlock);
	list_del(&file_data->list);
	list_is_empty = list_empty(&device_data->consuming_files);
	spin_unlock(&device_data->consumers_spinlock);

	// If there are no more users, detach the IRQ
	if (list_is_empty)
	{
		imsar_user_interrupt_detach_irq(device_data);
	}

	mutex_unlock(&device_data->irq_change_mutex);

	kfree(file_data);

	return 0;
}

static ssize_t imsar_user_interrupt_write(struct file *file, const char __user *buf, size_t bytes, loff_t *ppos)
{
	return -ENOTSUPP;
}

static ssize_t imsar_user_interrupt_read(struct file *file, char __user *buf, size_t bytes, loff_t *off)
{
	int status;
	imsar_user_interrupt_file_t *file_data;
	imsar_user_interrupt_device_t *device_data;
	int count;
	int timeout_jiffies;
	size_t actual_bytes;

	file_data = (imsar_user_interrupt_file_t *)file->private_data;
	device_data = (imsar_user_interrupt_device_t *)file_data->interrupt_dev;

	count = file_data->interrupt_count;

	if (count == 0)
	{
		if (file->f_flags & O_NONBLOCK)
		{
			return -EAGAIN;
		}
		else
		{
			timeout_jiffies = (file_data->timeout_ms * HZ) / 1000;
			status = wait_event_interruptible_timeout(file_data->file_waitqueue, (file_data->interrupt_count > 0),
			                                          timeout_jiffies);

			if (status == 0) // timeout
			{
				return -ETIME;
			}
			else if (status < 0) // interrupted
			{
				return status;
			}
		}
	}

	count = file_data->interrupt_count;

	file_data->interrupt_count = 0;

	actual_bytes = bytes > sizeof(count) ? sizeof(count) : bytes;
	status = copy_to_user(buf, &count, actual_bytes);
	if (status != 0)
	{
		dev_err(device_data->device, "read copy_to_user failed\n");
		return -EFAULT;
	}

	return actual_bytes;
}

static unsigned int imsar_user_interrupt_poll(struct file *file, poll_table *wait)
{
	imsar_user_interrupt_device_t *device_data;
	imsar_user_interrupt_file_t *file_data;
	unsigned int ret;

	file_data = (imsar_user_interrupt_file_t *)file->private_data;
	device_data = (imsar_user_interrupt_device_t *)file_data->interrupt_dev;

	// NOTE: this is NOT a blocking call -- this function (imsar_user_interrupt_poll)
	// will be called again when the wait queue is posted by the interrupt
	poll_wait(file, &file_data->file_waitqueue, wait);

	ret = 0;
	if (file_data->interrupt_count > 0)
	{
		ret = (POLLIN | POLLRDNORM);
	}
	return ret;
}

static long imsar_user_interrupt_ioctl(struct file *file, unsigned int request, unsigned long arg)
{
	int timeout_ms;
	int rc;
	imsar_user_interrupt_file_t *file_data;
	imsar_user_interrupt_device_t *device_data;

	file_data = (imsar_user_interrupt_file_t *)file->private_data;
	device_data = (imsar_user_interrupt_device_t *)file_data->interrupt_dev;

	switch (request)
	{
	case IMSAR_USER_INTERRUPT_IOCTL_TIMEOUT:
	{
		rc = copy_from_user(&timeout_ms, (const void __user *)arg, sizeof(int));
		if (rc)
		{
			return -EFAULT;
		}

		dev_info(device_data->device, "timeout: %d\n", timeout_ms);
		file_data->timeout_ms = (timeout_ms * HZ) / 1000;
		return 0;
	}
	case TCGETS: // ignore terminal commands
		return -EINVAL;
	default:
		dev_err(device_data->device, "unrecognized request %d\n", request);
		return -EINVAL;
	}
}

static int imsar_user_interrupt_char_dev_create(imsar_user_interrupt_device_t *device_data)
{
	int rc;

	// Allocate a character device region
	rc = alloc_chrdev_region(&device_data->char_dev_node, 0, 1, IMSAR_USER_INTERRUPT_DRIVER_NAME);
	if (rc)
	{
		dev_err(device_data->device, "alloc_chrdev_region failed\n");
		return rc;
	}

	// Initialize the character device data structure
	cdev_init(&device_data->char_dev, &imsar_user_interrupts_fops);
	device_data->char_dev.owner = THIS_MODULE;

	// Add the character device
	rc = cdev_add(&device_data->char_dev, device_data->char_dev_node, 1);
	if (rc)
	{
		dev_err(device_data->device, "unable to add char device\n");
		goto char_dev_fail;
	}

	// Create device node
	device_data->char_dev_device = device_create(s_device_class, &device_data->platform_dev->dev,
	                                             device_data->char_dev_node, device_data, "int_%s", device_data->name);

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

static void imsar_user_interrupt_char_dev_destroy(imsar_user_interrupt_device_t *device_data)
{
	if (!device_data->char_dev_device)
	{
		return;
	}
	device_destroy(s_device_class, device_data->char_dev_node);
	cdev_del(&device_data->char_dev);
	unregister_chrdev_region(device_data->char_dev_node, 1);
}

static int imsar_user_interrupt_device_data_init(struct platform_device *platform_dev,
                                                 imsar_user_interrupt_device_t *device_data)
{
	dev_set_drvdata(&platform_dev->dev, device_data);
	device_data->platform_dev = platform_dev;
	device_data->device = &platform_dev->dev;
	mutex_init(&device_data->irq_change_mutex);
	spin_lock_init(&device_data->consumers_spinlock);
	INIT_LIST_HEAD(&device_data->consuming_files);
	return 0;
}

static int imsar_user_interrupt_parse_dt(imsar_user_interrupt_device_t *device_data)
{
	int rc;

	// Read the name of the device
	rc = device_property_read_string(device_data->device, "imsar,name", &device_data->name);
	if (rc)
	{
		dev_err(device_data->device, "missing or invalid imsar,name property\n");
		return rc;
	}

	device_data->interrupt_name[0] = '\0';
	strlcat(device_data->interrupt_name, "int_", sizeof(device_data->interrupt_name));
	strlcat(device_data->interrupt_name, device_data->name, sizeof(device_data->interrupt_name));

	// Read the default timeout (ms)
	rc = device_property_read_u32_array(device_data->device, "imsar,default-timeout-ms",
	                                    &device_data->default_timeout_ms, 1);
	if (rc)
	{
		device_data->default_timeout_ms = 1000; // 1 second
	}

	dev_dbg(&device_data->platform_dev->dev, "name = %s", device_data->name);
	dev_dbg(&device_data->platform_dev->dev, "interrupt_name = %s", device_data->interrupt_name);
	dev_dbg(&device_data->platform_dev->dev, "default-timeout-ms = %u", device_data->default_timeout_ms);

	return 0;
}

static int imsar_user_interrupt_attach_irq(imsar_user_interrupt_device_t *device_data)
{
	int res;

	dev_dbg(device_data->device, "attaching IRQ");

	device_data->irq = platform_get_irq_optional(device_data->platform_dev, 0);
	if (device_data->irq < 0)
	{
		dev_err(device_data->device, "failed to get IRQ\n");
		return -EPROBE_DEFER;
	}

	res = devm_request_irq(device_data->device, device_data->irq, imsar_user_interrupt_handle_irq, 0,
	                       device_data->interrupt_name, device_data);
	if (res)
	{
		dev_err(device_data->device, "could not acquire IRQ\n");
		return -EPROBE_DEFER;
	}

	return 0;
}

static void imsar_user_interrupt_detach_irq(imsar_user_interrupt_device_t *device_data)
{
	dev_dbg(device_data->device, "detaching IRQ");

	devm_free_irq(device_data->device, device_data->irq, device_data);
}

static irqreturn_t imsar_user_interrupt_handle_irq(int num, void *dev_id)
{
	unsigned long flags;
	imsar_user_interrupt_file_t *entry;
	imsar_user_interrupt_device_t *device_data = (imsar_user_interrupt_device_t *)dev_id;

	spin_lock_irqsave(&device_data->consumers_spinlock, flags);

	list_for_each_entry(entry, &device_data->consuming_files, list)
	{
		entry->interrupt_count++;
		wake_up_interruptible_sync(&entry->file_waitqueue);
	}

	spin_unlock_irqrestore(&device_data->consumers_spinlock, flags);

	return IRQ_HANDLED;
}

static int imsar_user_interrupt_probe(struct platform_device *platform_dev)
{
	int ret;
	imsar_user_interrupt_device_t *device_data;

	dev_info(&platform_dev->dev, "IMSAR intc driver version: %s (%s)\n", GIT_DESCRIBE, BUILD_DATE);

	// Allocate and initialize device data
	device_data = devm_kzalloc(&platform_dev->dev, sizeof(imsar_user_interrupt_device_t), GFP_KERNEL);
	if (device_data == NULL)
	{
		return -ENOMEM;
	}

	ret = imsar_user_interrupt_device_data_init(platform_dev, device_data);
	if (ret < 0)
	{
		return ret;
	}

	ret = imsar_user_interrupt_parse_dt(device_data);
	if (ret < 0)
	{
		return ret;
	}

	// Create character device
	ret = imsar_user_interrupt_char_dev_create(device_data);
	if (ret != 0)
	{
		return ret;
	}

	return 0;
}

static int imsar_user_interrupt_remove(struct platform_device *platform_dev)
{
	imsar_user_interrupt_device_t *device_data = dev_get_drvdata(&platform_dev->dev);
	imsar_user_interrupt_char_dev_destroy(device_data);
	return 0;
}

static int __init imsar_user_interrupt_init(void)
{
	// Create device class
	s_device_class = class_create(THIS_MODULE, IMSAR_USER_INTERRUPT_DRIVER_NAME);
	if (IS_ERR(s_device_class))
	{
		return PTR_ERR(s_device_class);
	}

	s_device_class->dev_groups = imsar_user_interrupt_attr_groups;

	// Register platform driver
	return platform_driver_register(&imsar_user_interrupt_driver);
}

static void __exit imsar_user_interrupt_exit(void)
{
	// Unregister as platform driver
	platform_driver_unregister(&imsar_user_interrupt_driver);

	// Destroy the device class
	if (s_device_class)
	{
		class_destroy(s_device_class);
	}
}

ssize_t imsar_user_interrupt_name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	imsar_user_interrupt_device_t *device_data = (imsar_user_interrupt_device_t *)dev_get_drvdata(dev);
	if (!device_data || !device_data->name)
	{
		return 0;
	}
	return snprintf(buf, PAGE_SIZE, "%s\n", device_data->name);
}

module_init(imsar_user_interrupt_init);
module_exit(imsar_user_interrupt_exit);
