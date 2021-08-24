#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <linux/wait.h>
#include <asm/io.h>
#include <asm-generic/errno.h>
#include <asm/ioctls.h>

dev_t dev_num;
struct class *cls;
const char *CDEV_NAME = "intc";
#define MAX_INTERRUPTS 32

struct cdev_info {
	long int irq_count;
	long int timeout_ms;
	int irq;
	int valid;
	int open_count;
	wait_queue_head_t wq;

	struct platform_device *pdev;
	dev_t child_dev;
	struct cdev cdev;
};

struct cdev_info driver_data[MAX_INTERRUPTS];

static irqreturn_t irq_handler(int irq, void *devid)
{
	struct cdev_info *file_data = devid;
	pr_err("Got interrupt %d\n", irq);
	file_data->irq_count++;
	file_data->valid = true;
	wake_up_interruptible_sync(&file_data->wq);
	return IRQ_HANDLED;
}

ssize_t update_timeout(int index, const char *buf, size_t size)
{
	char *end;
	unsigned long timeout = simple_strtoul(buf, &end, 0);
	if (end == buf)
		return -EINVAL;

	driver_data[index].timeout_ms = timeout;
	// Always return full write size even if we didn't consume all
	return size;
}

///////////////////////////////////////////////////////////
// file ops
///////////////////////////////////////////////////////////
static ssize_t intc_write(struct file *f, const char __user *buf, size_t bytes, loff_t *ppos)
{
	int ii;
	ssize_t ret;
	char *kbuf;

	ii = iminor(f->f_inode);

	pr_info("file: write() %d\n", ii);

	kbuf = kmalloc(bytes, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;
	ret = copy_from_user(kbuf, buf, bytes);
	if (!ret) {
		kfree(kbuf);
		return -ENOMEM;
	}
	ret = update_timeout(ii, kbuf, bytes);
	kfree(kbuf);
	return ret;
}

static ssize_t intc_read(struct file *f, char __user *buf, size_t bytes, loff_t *off)
{
	int ii;
	int status;
	struct cdev_info *file_data = f->private_data;
	int current_count = file_data->irq_count;
	unsigned int timeout = (file_data->timeout_ms * CONFIG_HZ) / 1000;

	ii = iminor(f->f_inode);

	pr_info("file: read()  %d\n", ii);

	// wake me up once interrupt received
	status = wait_event_interruptible_timeout(file_data->wq,
						  (file_data->irq_count != current_count), timeout);

	if (status == 0) // timeout
		return -ETIME;
	else if (status < 0)
		return status;

	// clear valid once I'm awake
	file_data->valid = false;

	return 0;
}

static int intc_open(struct inode *inode, struct file *f)
{
	struct cdev_info *file_data;
	int ii = iminor(f->f_inode);
	int rv = 0;

	pr_info("file: open()  %d\n", ii);

	f->private_data = &driver_data[ii];
	file_data = f->private_data;

	if (!file_data->open_count)
		rv = request_irq(file_data->irq, irq_handler, IRQF_TRIGGER_RISING,
				 dev_name(&file_data->pdev->dev), file_data);
	file_data->open_count++;

	return rv;
}

static int intc_close(struct inode *inode, struct file *f)
{
	struct cdev_info *file_data = f->private_data;
	int ii = iminor(f->f_inode);

	pr_info("file: close()  %d\n", ii);

	file_data->open_count--;
	if (!file_data->open_count)
		free_irq(file_data->irq, f->private_data);

	return 0;
}

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = intc_open,
	.release = intc_close,
	.write = intc_write,
	.read = intc_read,
	//        .unlocked_ioctl = intc_ioctl
};

///////////////////////////////////////////////////////////
// sysfs attributes
///////////////////////////////////////////////////////////
ssize_t intc_timeout_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	s32 milliseconds;
	milliseconds = driver_data[MINOR(dev->devt)].timeout_ms;
	return snprintf(buf, PAGE_SIZE, "%d\n", milliseconds);
}

ssize_t intc_timeout_store(struct device *dev, struct device_attribute *attr, const char *buf,
			   size_t size)
{
	return update_timeout(MINOR(dev->devt), buf, size);
}

ssize_t intc_count_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%ld\n", driver_data[MINOR(dev->devt)].irq_count);
}

static DEVICE_ATTR(count, S_IRUGO, intc_count_show, NULL);
static DEVICE_ATTR(default_timeout_ms, (S_IRUGO | S_IWUSR | S_IWGRP), intc_timeout_show,
		   intc_timeout_store);

static struct attribute *attrs[] = {
	&dev_attr_count.attr,
	&dev_attr_default_timeout_ms.attr,
	NULL,
};
static struct attribute_group attr_group = {
	.attrs = attrs,
};
static const struct attribute_group *attr_groups[] = {
	&attr_group,
	NULL,
};

///////////////////////////////////////////////////////////
// Setup/Teardown
///////////////////////////////////////////////////////////
static int intc_of_probe(struct platform_device *pdev)
{
	struct cdev_info *file_data;
	struct device *device;
	u32 index;
	u32 milliseconds;
	int ret;

	ret = of_property_read_u32(pdev->dev.of_node, "interrupts", &index);
	if (ret < 0) {
		pr_err("No interrupts property on interrupt node\n");
		return -ERANGE;
	}
	file_data = &driver_data[index];

	init_waitqueue_head(&file_data->wq);
	file_data->irq_count = 0;
	file_data->open_count = 0;
	file_data->pdev = pdev;
	file_data->timeout_ms = MAX_SCHEDULE_TIMEOUT;
	file_data->valid = false;

	ret = of_property_read_u32(pdev->dev.of_node, "timeout_ms", &milliseconds);
	if (ret < 0) {
		pr_info("no property timeout for interrupt node\n");
	} else {
		pr_info("interrupt #%d timeout = %d\n", index, milliseconds);
		file_data->timeout_ms = milliseconds;
	}

	file_data->irq = platform_get_irq(pdev, 0);
	if (file_data->irq < 0)
		return file_data->irq;
	if (file_data->irq == 0)
		return -EINVAL;

	file_data->child_dev = MKDEV(MAJOR(dev_num), MINOR(dev_num) + index);

	device = device_create(cls, NULL, file_data->child_dev, file_data, "intc_%s",
			       pdev->dev.of_node->name);
	if (IS_ERR_OR_NULL(device)) {
		pr_err("Unable to create device\n");
		return -EIO;
	}

	cdev_init(&file_data->cdev, &fops);
	if (cdev_add(&file_data->cdev, file_data->child_dev, 1)) {
		device_destroy(cls, file_data->child_dev);
		return -EIO;
	}

	platform_set_drvdata(pdev, file_data);
	return 0;
}

static int intc_of_remove(struct platform_device *pdev)
{
	struct cdev_info *dmac = platform_get_drvdata(pdev);
	if (dmac) {
		cdev_del(&dmac->cdev);
		device_destroy(cls, dmac->child_dev);
	}
	return 0;
}

static const struct of_device_id intc_of_match[] = {
	{
		.compatible = "imsar,intc",
	},
	{ /* end of list */ },
};
MODULE_DEVICE_TABLE(of, intc_of_match);

static struct platform_driver intc_of_driver = {
		.probe = intc_of_probe,
		.remove = intc_of_remove,
		.driver = {
				.name = "interrupt_cdev",
				.owner = THIS_MODULE,
				.of_match_table = intc_of_match,
		},
};
// module_platform_driver(intc_of_driver);
static int __init intc_of_driver_init(void)
{
	int rv = -EIO;

	if ((rv = alloc_chrdev_region(&dev_num, 0, MAX_INTERRUPTS, CDEV_NAME))) {
		pr_err("Unable to allocate cdev region %d.\n", rv);
		goto cleanup_mem;
	}

	cls = class_create(THIS_MODULE, CDEV_NAME);
	if (!cls) {
		pr_err("Unable to create cdev class\n");
		rv = -ENOMEM;
		goto cleanup_chrdev;
	}
	cls->dev_groups = attr_groups;

	rv = platform_driver_register(&(intc_of_driver));
	if (rv)
		goto cleanup_class;
	return rv;

cleanup_class:
	class_destroy(cls);
cleanup_chrdev:
	unregister_chrdev_region(dev_num, MAX_INTERRUPTS);
cleanup_mem:
	return rv;
}
module_init(intc_of_driver_init);
static void __exit intc_of_driver_exit(void)
{
	platform_driver_unregister(&(intc_of_driver));
	class_destroy(cls);
	unregister_chrdev_region(dev_num, MAX_INTERRUPTS);
}
module_exit(intc_of_driver_exit);

MODULE_LICENSE("GPL");