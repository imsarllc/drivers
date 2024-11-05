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

dev_t imsar_pcie_intc_dev_num;
struct class *imsar_pcie_intc_cls;
const char *IMSAR_PCIE_INTC_CDEV_NAME = "intc";
#define MAX_INTERRUPTS 32

struct imsar_pcie_intc_cdev_info {
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

struct imsar_pcie_intc_cdev_info imsar_pcie_intc_driver_data[MAX_INTERRUPTS];

static irqreturn_t imsar_pcie_intc_irq_handler(int irq, void *devid)
{
	struct imsar_pcie_intc_cdev_info *file_data = devid;
	pr_err("Got interrupt %d\n", irq);
	file_data->irq_count++;
	file_data->valid = true;
	wake_up_interruptible_sync(&file_data->wq);
	return IRQ_HANDLED;
}

ssize_t imsar_pcie_intc_update_timeout(int index, const char *buf, size_t size)
{
	char *end;
	unsigned long timeout = simple_strtoul(buf, &end, 0);
	if (end == buf)
		return -EINVAL;

	imsar_pcie_intc_driver_data[index].timeout_ms = timeout;
	// Always return full write size even if we didn't consume all
	return size;
}

///////////////////////////////////////////////////////////
// file ops
///////////////////////////////////////////////////////////
static ssize_t imsar_pcie_intc_intc_write(struct file *f, const char __user *buf, size_t bytes,
					  loff_t *ppos)
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
	ret = imsar_pcie_intc_update_timeout(ii, kbuf, bytes);
	kfree(kbuf);
	return ret;
}

static ssize_t imsar_pcie_intc_intc_read(struct file *f, char __user *buf, size_t bytes,
					 loff_t *off)
{
	int ii;
	int status;
	struct imsar_pcie_intc_cdev_info *file_data = f->private_data;
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

static int imsar_pcie_intc_intc_open(struct inode *inode, struct file *f)
{
	struct imsar_pcie_intc_cdev_info *file_data;
	int ii = iminor(f->f_inode);
	int rv = 0;

	pr_info("file: open()  %d\n", ii);

	f->private_data = &imsar_pcie_intc_driver_data[ii];
	file_data = f->private_data;

	if (!file_data->open_count)
		rv = request_irq(file_data->irq, imsar_pcie_intc_irq_handler, IRQF_TRIGGER_RISING,
				 dev_name(&file_data->pdev->dev), file_data);
	file_data->open_count++;

	return rv;
}

static int imsar_pcie_intc_intc_close(struct inode *inode, struct file *f)
{
	struct imsar_pcie_intc_cdev_info *file_data = f->private_data;
	int ii = iminor(f->f_inode);

	pr_info("file: close()  %d\n", ii);

	file_data->open_count--;
	if (!file_data->open_count)
		free_irq(file_data->irq, f->private_data);

	return 0;
}

static struct file_operations imsar_pcie_intc_fops = {
	.owner = THIS_MODULE,
	.open = imsar_pcie_intc_intc_open,
	.release = imsar_pcie_intc_intc_close,
	.write = imsar_pcie_intc_intc_write,
	.read = imsar_pcie_intc_intc_read,
	//        .unlocked_ioctl = intc_ioctl
};

///////////////////////////////////////////////////////////
// sysfs attributes
///////////////////////////////////////////////////////////
ssize_t imsar_pcie_intc_timeout_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	s32 milliseconds;
	milliseconds = imsar_pcie_intc_driver_data[MINOR(dev->devt)].timeout_ms;
	return snprintf(buf, PAGE_SIZE, "%d\n", milliseconds);
}

ssize_t imsar_pcie_intc_timeout_store(struct device *dev, struct device_attribute *attr,
				      const char *buf, size_t size)
{
	return imsar_pcie_intc_update_timeout(MINOR(dev->devt), buf, size);
}

ssize_t imsar_pcie_intc_count_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%ld\n",
			imsar_pcie_intc_driver_data[MINOR(dev->devt)].irq_count);
}

static DEVICE_ATTR(count, S_IRUGO, imsar_pcie_intc_count_show, NULL);
static DEVICE_ATTR(default_timeout_ms, (S_IRUGO | S_IWUSR | S_IWGRP), imsar_pcie_intc_timeout_show,
		   imsar_pcie_intc_timeout_store);

static struct attribute *imsar_pcie_intc_attrs[] = {
	&dev_attr_count.attr,
	&dev_attr_default_timeout_ms.attr,
	NULL,
};
static struct attribute_group imsar_pcie_intc_attr_group = {
	.attrs = imsar_pcie_intc_attrs,
};
static const struct attribute_group *imsar_pcie_intc_attr_groups[] = {
	&imsar_pcie_intc_attr_group,
	NULL,
};

///////////////////////////////////////////////////////////
// Setup/Teardown
///////////////////////////////////////////////////////////
static int imsar_pcie_intc_probe(struct platform_device *pdev)
{
	struct imsar_pcie_intc_cdev_info *file_data;
	struct device *device;
	u32 index;
	u32 milliseconds;
	int ret;

	ret = of_property_read_u32(pdev->dev.of_node, "interrupts", &index);
	if (ret < 0) {
		pr_err("No interrupts property on interrupt node\n");
		return -ERANGE;
	}
	file_data = &imsar_pcie_intc_driver_data[index];

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

	file_data->child_dev =
		MKDEV(MAJOR(imsar_pcie_intc_dev_num), MINOR(imsar_pcie_intc_dev_num) + index);

	device = device_create(imsar_pcie_intc_cls, NULL, file_data->child_dev, file_data,
			       "intc_%s", pdev->dev.of_node->name);
	if (IS_ERR_OR_NULL(device)) {
		pr_err("Unable to create device\n");
		return -EIO;
	}

	cdev_init(&file_data->cdev, &imsar_pcie_intc_fops);
	if (cdev_add(&file_data->cdev, file_data->child_dev, 1)) {
		device_destroy(imsar_pcie_intc_cls, file_data->child_dev);
		return -EIO;
	}

	platform_set_drvdata(pdev, file_data);
	return 0;
}

static int imsar_pcie_intc_remove(struct platform_device *pdev)
{
	struct imsar_pcie_intc_cdev_info *dmac = platform_get_drvdata(pdev);
	if (dmac) {
		cdev_del(&dmac->cdev);
		device_destroy(imsar_pcie_intc_cls, dmac->child_dev);
	}
	return 0;
}

static const struct of_device_id imsar_pcie_intc_of_match[] = {
	{
		.compatible = "imsar,intc",
	},
	{ /* end of list */ },
};
MODULE_DEVICE_TABLE(of, imsar_pcie_intc_of_match);

static struct platform_driver imsar_pcie_intc_driver = {
		.probe = imsar_pcie_intc_probe,
		.remove = imsar_pcie_intc_remove,
		.driver = {
				.name = "imsar_pcie_intc",
				.owner = THIS_MODULE,
				.of_match_table = imsar_pcie_intc_of_match,
		},
};
// module_platform_driver(imsar_pcie_intc_driver);
static int __init imsar_pcie_intc_init(void)
{
	int rv = -EIO;

	if ((rv = alloc_chrdev_region(&imsar_pcie_intc_dev_num, 0, MAX_INTERRUPTS,
				      IMSAR_PCIE_INTC_CDEV_NAME))) {
		pr_err("Unable to allocate cdev region %d.\n", rv);
		goto cleanup_mem;
	}

	imsar_pcie_intc_cls = class_create(THIS_MODULE, IMSAR_PCIE_INTC_CDEV_NAME);
	if (!imsar_pcie_intc_cls) {
		pr_err("Unable to create cdev class\n");
		rv = -ENOMEM;
		goto cleanup_chrdev;
	}
	imsar_pcie_intc_cls->dev_groups = imsar_pcie_intc_attr_groups;

	rv = platform_driver_register(&(imsar_pcie_intc_driver));
	if (rv)
		goto cleanup_class;
	return rv;

cleanup_class:
	class_destroy(imsar_pcie_intc_cls);
cleanup_chrdev:
	unregister_chrdev_region(imsar_pcie_intc_dev_num, MAX_INTERRUPTS);
cleanup_mem:
	return rv;
}

static void __exit imsar_pcie_intc_exit(void)
{
	platform_driver_unregister(&(imsar_pcie_intc_driver));
	class_destroy(imsar_pcie_intc_cls);
	unregister_chrdev_region(imsar_pcie_intc_dev_num, MAX_INTERRUPTS);
}

module_init(imsar_pcie_intc_init);
module_exit(imsar_pcie_intc_exit);

MODULE_LICENSE("GPL");