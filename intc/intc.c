#include "intc.h"

#include "version.h"
#include <asm-generic/errno.h>
#include <asm/io.h>
#include <asm/ioctls.h>
#include <linux/cdev.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/pagemap.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/wait.h>

#define INTC_IRQ_COUNT 16
#define INTC_EN_OFFSET 0x0000
#define INTC_MASK_OFFSET 0x0001
#define INTC_SET_OFFSET 0x0002
#define INTC_CLR_OFFSET 0x0003
#define INTC_PEND_OFFSET 0x0004
#define INTC_ADDR_MASK 0x0fff
#define INTC_DATA_INVALID 0xbeef

#define DEVICE_NAME "intc"

typedef struct
{
	// interrupt specific
	const char *name;
	unsigned long interrupt_count;
	signed long default_timeout;      // in jiffies
	spinlock_t consumers_spinlock;    // held when changing consuming_files
	struct list_head consuming_files; // points at intc_file_t entries
} intc_dev_t;

typedef struct
{
	unsigned long interrupt_count;
	signed long timeout_jiffies; // in jiffies
	wait_queue_head_t file_waitqueue;
	enum intc_behavior behavior;
	struct list_head list; // used to link pointers for consuming_files
} intc_file_t;

// globals
static struct device *logging_device;
static intc_dev_t fid[INTC_IRQ_COUNT]; // FPGA interrupt device
static void *vbase;                    // virtual address to FPGA intc regs base address
static int irq;                        // request number
static int irqnum;                     // grant number

static struct cdev cdev;
static dev_t dev;
static struct class *cl;


static void intc_reg_set(unsigned short addr, unsigned short mask);
static void intc_reg_clear(unsigned short addr, unsigned short mask);
static void intc_reg_write(unsigned int addr, u16 data);
static u16 intc_reg_read(unsigned int addr);

static int intc_consumer_add(intc_dev_t *device_data, intc_file_t *file_data);
static int intc_consumer_remove(intc_dev_t *device_data, intc_file_t *file_data);

static int intc_enable(int intc_line, int enable)
{
	if (enable)
	{
		intc_reg_set(INTC_EN_OFFSET, 1 << intc_line);
	}
	else
	{
		intc_reg_clear(INTC_EN_OFFSET, 1 << intc_line);
	}

	return 0;
}

static ssize_t intc_write(struct file *f, const char __user *buf, size_t bytes, loff_t *ppos)
{
	int ii = iminor(f->f_inode);
	intc_file_t *file_data = (intc_file_t *)f->private_data;
	dev_dbg(logging_device, "file: write() %d\n", ii);
	file_data->interrupt_count = 0;
	return bytes;
}

static ssize_t intc_read(struct file *f, char __user *buf, size_t bytes, loff_t *off)
{
	int status;
	int ii;
	intc_file_t *file_data;
	intc_dev_t *dev_data;
	int count;
	int timeout_jiffies;

	ii = iminor(f->f_inode);
	file_data = (intc_file_t *)f->private_data;
	dev_data = &fid[ii];

	dev_dbg(logging_device, "file: read()  %d\n", ii);

	if (file_data->behavior == INTC_BEHAVIOR_NEXT_ONLY)
	{
		if (f->f_flags & O_NONBLOCK)
		{
			dev_err(logging_device, "non-blocking read not supported with behavior NEXT_ONLY");
			return -ENOTSUPP;
		}

		file_data->interrupt_count = 0;
	}

	if (f->f_flags & O_NONBLOCK)
	{
		timeout_jiffies = 0; // immediate timeout
	}
	else
	{
		timeout_jiffies = file_data->timeout_jiffies;
	}

	status = wait_event_interruptible_timeout(file_data->file_waitqueue, //
	                                          (file_data->interrupt_count > 0), timeout_jiffies);

	if (status == 0) // timeout
	{
		return (f->f_flags & O_NONBLOCK) ? -EAGAIN : -ETIME;
	}
	else if (status < 0) // interrupted
	{
		return status;
	}

	count = file_data->interrupt_count;

	file_data->interrupt_count = 0;

	if (bytes == sizeof(count))
	{
		status = copy_to_user(buf, &count, bytes);
		if (status != 0)
		{
			dev_err(logging_device, "read copy_to_user failed\n");
			return -EFAULT;
		}
	}

	return 0;
}

static unsigned int intc_poll(struct file *file, poll_table *wait)
{
	intc_file_t *file_data;
	intc_dev_t *dev_data;
	unsigned int ret;
	int ii;

	ii = iminor(file->f_inode);
	file_data = (intc_file_t *)file->private_data;
	dev_data = &fid[ii];

	ret = 0;

	if (file_data->behavior == INTC_BEHAVIOR_NEXT_ONLY)
	{
		dev_err(logging_device, "poll not supported with behavior NEXT_ONLY");
		return -ENOTSUPP;
	}

	if (file_data->interrupt_count > 0)
	{
		ret = (POLLIN | POLLRDNORM);
	}
	else
	{
		// NOTE: this is NOT a blocking call -- this function (intc_poll)
		// will be called again when the wait queue is posted by the interrupt
		poll_wait(file, &file_data->file_waitqueue, wait);
	}

	return ret;
}

static int intc_consumer_add(intc_dev_t *device_data, intc_file_t *file_data)
{
	int list_was_empty;

	spin_lock(&device_data->consumers_spinlock);
	list_was_empty = list_empty(&device_data->consuming_files);
	list_add_tail(&file_data->list, &device_data->consuming_files);
	spin_unlock(&device_data->consumers_spinlock);
	return list_was_empty;
}

static int intc_consumer_remove(intc_dev_t *device_data, intc_file_t *file_data)
{
	int list_is_now_empty;

	spin_lock(&device_data->consumers_spinlock);
	list_del(&file_data->list);
	list_is_now_empty = list_empty(&device_data->consuming_files);
	spin_unlock(&device_data->consumers_spinlock);
	return list_is_now_empty;
}

static irqreturn_t intc_isr(int num, void *dev_id)
{
	unsigned short pending;
	unsigned long flags;
	int ii;
	intc_dev_t *intc_dev;
	intc_file_t *entry;

	intc_reg_write(INTC_MASK_OFFSET, 1);       // mask all interrupts
	pending = intc_reg_read(INTC_PEND_OFFSET); // determine pending IRQ

	// address FPGA IRQ interrupts, right to left
	for (ii = 0; ii < INTC_IRQ_COUNT; ii++)
	{
		if ((pending & (1 << ii)) == 0)
		{
			continue;
		}

		intc_dev = &fid[ii];
		intc_dev->interrupt_count++;

		spin_lock_irqsave(&intc_dev->consumers_spinlock, flags);

		list_for_each_entry(entry, &intc_dev->consuming_files, list)
		{
			entry->interrupt_count++;
			wake_up_interruptible_sync(&entry->file_waitqueue);
		}

		spin_unlock_irqrestore(&intc_dev->consumers_spinlock, flags);
	}

	intc_reg_write(INTC_CLR_OFFSET, pending); // clear interrupts
	intc_reg_write(INTC_MASK_OFFSET, 0);      // unmask

	return IRQ_HANDLED;
}

static long intc_irq_init(int num)
{
	if (irqnum)
	{
		free_irq(irqnum, NULL); // free previously requested IRQ
	}

	irqnum = num;
	if (request_irq(irqnum, intc_isr, 0, DEVICE_NAME, NULL))
	{
		dev_err(logging_device, "unable to register IRQ %d\n", irqnum);
		irqnum = 0;
		return -EIO;
	}

	return 0;
}

static void intc_reset(void)
{
	intc_reg_write(INTC_EN_OFFSET, 0);
	intc_reg_write(INTC_CLR_OFFSET, 0xffff);
}

static long intc_ioctl(struct file *f, unsigned int request, unsigned long arg)
{
	int ret = 0;
	int ii;
	intc_dev_t *dev_data;

	intc_file_t *file_data = (intc_file_t *)f->private_data;

	ii = iminor(f->f_inode);
	dev_data = &fid[ii];

	switch (request)
	{
	case INTC_INT_COUNT:
	{
		dev_info(logging_device, "file: ioctl() %d, interrupt count:%ld\n", ii, dev_data->interrupt_count);
		ret = dev_data->interrupt_count;
		break;
	}
	case INTC_ENABLE:
	{
		int enable;
		if (copy_from_user((void *)&enable, (const void __user *)arg, sizeof(int)))
		{
			return -EFAULT;
		}

		dev_info(logging_device, "file: ioctl() %d, enable:%d\n", ii, enable);
		ret = intc_enable(ii, enable);
		break;
	}
	case INTC_TIMEOUT:
	{
		int milliseconds;
		if (copy_from_user((void *)&milliseconds, (const void __user *)arg, sizeof(int)))
		{
			return -EFAULT;
		}

		dev_info(logging_device, "file: ioctl() %d, timeout:%d\n", ii, milliseconds);
		file_data->timeout_jiffies = (milliseconds * HZ) / 1000;
		break;
	}
	case INTC_BEHAVIOR:
	{
		int tmp;
		if (copy_from_user(&tmp, (const void __user *)arg, sizeof(int)))
		{
			return -EFAULT;
		}
		dev_info(logging_device, "file: ioctl() %d, behavior:%d\n", ii, tmp);
		file_data->behavior = tmp;
		return 0;
	}
	case TCGETS:
		// Silently ignore terminal commands
		ret = -EINVAL;
		break;
	default:
		dev_err(logging_device, "file: ioctl() %d, unrecognized request %d\n", ii, request);
		ret = -EINVAL;
		break;
	}

	if (copy_to_user((unsigned int *)arg, &ret, sizeof(int)))
	{
		return -EFAULT;
	}

	return 0;
}

static int intc_open(struct inode *inode, struct file *f)
{
	intc_file_t *file_data;
	intc_dev_t *dev_data;
	int list_was_empty;
	int ii = iminor(f->f_inode);

	dev_data = &fid[ii];
	file_data = kzalloc(sizeof(intc_file_t), GFP_KERNEL);
	if (file_data == NULL)
	{
		dev_err(logging_device, "kmalloc failed\n");
		return -ENOMEM;
	}
	f->private_data = file_data;

	file_data->timeout_jiffies = dev_data->default_timeout;
	init_waitqueue_head(&file_data->file_waitqueue);
	INIT_LIST_HEAD(&file_data->list);
	file_data->interrupt_count = 0;
	file_data->behavior = INTC_BEHAVIOR_NEXT_ONLY;

	list_was_empty = intc_consumer_add(dev_data, file_data);

	if (list_was_empty)
	{
		intc_enable(ii, 1);
	}

	return 0;
}

static int intc_close(struct inode *inode, struct file *f)
{
	bool list_is_empty;
	intc_file_t *file_data;
	intc_dev_t *dev_data;
	int ii = iminor(f->f_inode);

	dev_data = &fid[ii];
	file_data = f->private_data;

	list_is_empty = intc_consumer_remove(dev_data, file_data);
	if (list_is_empty)
	{
		intc_enable(ii, 0);
	}

	kfree(file_data);

	dev_dbg(logging_device, "<%s> file: close() %d\n", DEVICE_NAME, ii);

	return 0;
}

static struct file_operations fops = {.owner = THIS_MODULE,
                                      .open = intc_open,
                                      .release = intc_close,
                                      .write = intc_write,
                                      .read = intc_read,
                                      .poll = intc_poll,
                                      .unlocked_ioctl = intc_ioctl};

ssize_t intc_name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	intc_dev_t *dev_data = &fid[MINOR(dev->devt)];
	if (dev_data->name)
	{
		return snprintf(buf, PAGE_SIZE, "%s\n", dev_data->name);
	}
	else
	{
		return 0;
	}
}

ssize_t intc_count_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	intc_dev_t *dev_data = &fid[MINOR(dev->devt)];
	return snprintf(buf, PAGE_SIZE, "%ld\n", dev_data->interrupt_count);
}

ssize_t intc_timeout_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	s32 milliseconds;
	intc_dev_t *dev_data = &fid[MINOR(dev->devt)];
	milliseconds = dev_data->default_timeout * 1000 / HZ;
	return snprintf(buf, PAGE_SIZE, "%d\n", milliseconds);
}

ssize_t intc_timeout_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	char *end;
	s32 timeout;
	intc_dev_t *dev_data = &fid[MINOR(dev->devt)];
	unsigned long new = simple_strtoul(buf, &end, 0);
	if (end == buf)
	{
		return -EINVAL;
	}

	timeout = (new *HZ) / 1000;
	dev_data->default_timeout = timeout;
	// Always return full write size even if we didn't consume all
	return size;
}

ssize_t intc_enable_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int enabled = 0;
	unsigned short res;
	res = intc_reg_read(INTC_EN_OFFSET) & (1 << MINOR(dev->devt));
	if (res)
	{
		enabled = 1;
	}
	return snprintf(buf, PAGE_SIZE, "%d\n", enabled);
}

ssize_t intc_enable_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	char *end;
	unsigned long enable = simple_strtoul(buf, &end, 0);
	if (end == buf || (enable != 1 && enable != 0))
		return -EINVAL;

	intc_enable(MINOR(dev->devt), enable);
	// Always return full write size even if we didn't consume all
	return size;
}

// TODO: This should be a debugfs entry instead.
ssize_t intc_set_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	intc_reg_write(INTC_SET_OFFSET, 0);
	intc_reg_set(INTC_SET_OFFSET, 1 << MINOR(dev->devt));
	// Always return full write size even if we didn't consume all
	return size;
}

static DEVICE_ATTR(name, S_IRUGO, intc_name_show, NULL);
static DEVICE_ATTR(count, S_IRUGO, intc_count_show, NULL);
static DEVICE_ATTR(default_timeout_ms, (S_IRUGO | S_IWUSR | S_IWGRP), intc_timeout_show, intc_timeout_store);
static DEVICE_ATTR(enable, (S_IRUGO | S_IWUSR | S_IWGRP), intc_enable_show, intc_enable_store);
static DEVICE_ATTR(set, S_IWUSR, NULL, intc_set_store);

static struct attribute *attrs[] = {
    &dev_attr_name.attr,   &dev_attr_count.attr, &dev_attr_default_timeout_ms.attr,
    &dev_attr_enable.attr, &dev_attr_set.attr,   NULL,
};

static struct attribute_group attr_group = {
    .attrs = attrs,
};

static const struct attribute_group *attr_groups[] = {
    &attr_group,
    NULL,
};

static int intc_chrdev_init(void)
{
	int ii;
	if (alloc_chrdev_region(&dev, 0, INTC_IRQ_COUNT, DEVICE_NAME) < 0)
	{
		dev_err(logging_device, "init: alloc_chrdev_region failed\n");
		return -1;
	}
	if ((cl = class_create(THIS_MODULE, DEVICE_NAME)) == NULL)
	{
		dev_err(logging_device, "init: class_create failed\n");
		unregister_chrdev_region(dev, INTC_IRQ_COUNT);
		return -1;
	}

	cl->dev_groups = attr_groups;

	for (ii = 0; ii < INTC_IRQ_COUNT; ii++)
	{
		if (device_create(cl, NULL, MKDEV(MAJOR(dev), MINOR(dev) + ii), NULL, "intc%d", ii) == NULL)
		{
			dev_err(logging_device, "init: device_create failed\n");
			class_destroy(cl);
			unregister_chrdev_region(dev, INTC_IRQ_COUNT);
			return -1;
		}
	}

	cdev_init(&cdev, &fops);
	if (cdev_add(&cdev, dev, INTC_IRQ_COUNT) == -1)
	{
		dev_err(logging_device, "init: cdev_add failed\n");
		device_destroy(cl, dev);
		class_destroy(cl);
		unregister_chrdev_region(dev, INTC_IRQ_COUNT);
		return -1;
	}
	return 0;
}

static int intc_of_remove(struct platform_device *of_dev)
{
	int ii;

	iounmap(vbase);
	cdev_del(&cdev);
	for (ii = 0; ii < INTC_IRQ_COUNT; ii++)
	{
		device_destroy(cl, MKDEV(MAJOR(dev), MINOR(dev) + ii));
	}
	class_destroy(cl);
	unregister_chrdev_region(dev, INTC_IRQ_COUNT);
	dev_info(logging_device, "unregistered\n");

	free_irq(irqnum, NULL);
	irqnum = 0;

	return 0;
}

static int intc_of_probe(struct platform_device *ofdev)
{
	int ret, ii;
	struct device_node *child;
	struct resource *res;
	resource_size_t size;

	logging_device = &ofdev->dev;

	dev_info(logging_device, "%s version: %s (%s)\n", "IMSAR intc driver", GIT_DESCRIBE, BUILD_DATE);

	res = platform_get_resource(ofdev, IORESOURCE_IRQ, 0);
	if (!res)
	{
		dev_err(logging_device, "could not get platform IRQ resource\n");
		return -1;
	}

	irq = res->start;
	dev_info(logging_device, "probe: IRQ read from DTS entry as %d\n", irq);
	ret = intc_irq_init(irq);
	if (ret != 0)
	{
		return ret;
	}

	res = platform_get_resource(ofdev, IORESOURCE_MEM, 0);
	size = resource_size(res);

	dev_dbg(logging_device, "probe: register physical base address = %x\n", res->start);

	vbase = of_iomap(ofdev->dev.of_node, 0);
	if (!vbase)
	{
		dev_err(logging_device, "of_iomap failed for resource %pR\n", res);
		return -ENOMEM;
	}

	// register the character device
	if ((ret = intc_chrdev_init()) != 0)
	{
		dev_err(logging_device, "failed to register character device\n");
		return ret;
	}

	for (ii = 0; ii < INTC_IRQ_COUNT; ii++)
	{
		intc_dev_t *dev_data = &fid[ii];
		// initialize interrupts
		dev_data->name = NULL;
		dev_data->interrupt_count = 0;
		dev_data->default_timeout = MAX_SCHEDULE_TIMEOUT; // disable timeouts
		spin_lock_init(&dev_data->consumers_spinlock);
		INIT_LIST_HEAD(&dev_data->consuming_files);
	}

	for_each_child_of_node(ofdev->dev.of_node, child)
	{
		u32 index;
		s32 milliseconds;

		ret = of_property_read_u32(child, "reg", &index);
		if (ret < 0)
		{
			dev_info(logging_device, "no property reg for child of FPGA interrupt controller\n");
		}
		else
		{
			fid[index].name = child->name;
			dev_dbg(logging_device, "interrupt #%d = %s\n", index, child->name);
		}

		ret = of_property_read_u32(child, "timeout_ms", &milliseconds);
		if (ret < 0)
		{
			dev_info(logging_device, "no property timeout for child of FPGA interrupt controller\n");
		}
		else
		{
			dev_dbg(logging_device, "interrupt #%d timeout = %d\n", index, milliseconds);
			fid[index].default_timeout = (milliseconds * HZ) / 1000;
		}
	}

	intc_reset();

	dev_info(logging_device, "registered\n");

	return 0;
}

static void intc_reg_write(unsigned int addr, u16 data)
{
	iowrite16(data, vbase + addr * 4);
}

static u16 intc_reg_read(unsigned int addr)
{
	return ioread16(vbase + addr * 4);
}

static void intc_reg_set(unsigned short addr, unsigned short mask)
{
	unsigned short data;
	data = intc_reg_read(addr);
	intc_reg_write(addr, data | mask);
}

static void intc_reg_clear(unsigned short addr, unsigned short mask)
{
	unsigned short data;
	data = intc_reg_read(addr);
	intc_reg_write(addr, data & ~mask);
}

static const struct of_device_id intc_of_match[] = {
    {
        .compatible = "imsar,intc",
    },
    {/* end of list */},
};
MODULE_DEVICE_TABLE(of, intc_of_match);

static struct platform_driver intc_of_driver = {
    .probe = intc_of_probe,
    .remove = intc_of_remove,
    .driver =
        {
            .name = DEVICE_NAME,
            .owner = THIS_MODULE,
            .of_match_table = intc_of_match,
        },
};
module_platform_driver(intc_of_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("IMSAR Embedded Team <embedded@imsar.com>");
MODULE_DESCRIPTION("Driver for ImSAR FPGA interrupt controller");
MODULE_VERSION(GIT_DESCRIBE);
