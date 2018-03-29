#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <asm/io.h>
#include "intc.h"
#include "version.h"

#define FPGA_REGS_SIZE      0x00010000
#define INTC_IRQ_COUNT      16
#define INTC_EN_OFFSET      0x0000
#define INTC_MASK_OFFSET    0x0001
#define INTC_SET_OFFSET     0x0002
#define INTC_CLR_OFFSET     0x0003
#define INTC_PEND_OFFSET    0x0004
#define INTC_ADDR_MASK      0x0fff
#define INTC_DATA_INVALID   0xbeef

// macros
#define DEBUG 0
#define DEVICE_NAME  "intc"

#if DEBUG
#define intc_reg_write(addr, data) \
	{ \
		iowrite16((data), vbase + (addr) * 4); \
		printk(KERN_DEBUG "<%s> wr addr:%04x data:%04x\n", DEVICE_NAME, addr, data); \
	}
#define intc_reg_read(data, addr) \
	{ \
		data = ioread16(vbase + (addr) * 4); \
		printk(KERN_DEBUG "<%s> rd addr:%04x data:%04x\n", DEVICE_NAME, addr, data); \
	}
#else
#define intc_reg_write(addr, data)  iowrite16((data), vbase + (addr) * 4)
#define intc_reg_read(data, addr)   data = ioread16(vbase + (addr) * 4)
#endif // DEBUG
#define fpga_reg_read(addr)         ioread16(vfpga + (addr) * 4);

typedef struct {
	// interrupt specific
	bool valid; // indicates fresh interrupt
	bool open;
	int rcnt;   // number of addresses to read on interrupt
	unsigned short *addr; // kmalloc'd array of addresses
	unsigned short *data; // values read on interrupt to be copied to user
	const char* name;
	signed long timeout; // in jiffies
	unsigned long count;
} intc_dev_t;

// globals
static intc_dev_t fid[INTC_IRQ_COUNT]; // FPGA interrupt device
static DECLARE_WAIT_QUEUE_HEAD( wq); // wait queue used to wake up read() from ISR
static void *vfpga;       // virtual address to FPGA regs base address
static void *vbase;       // virtual address to FPGA intc regs base address
static int irq;           // request number
static int irqnum;        // grant number
static spinlock_t ilock;  // spinlock used for interrupt handler
static int open_files = 0;

static struct cdev cdev;
static dev_t dev;
static struct class *cl;

static void intc_reg_set(unsigned short addr, unsigned short mask)
{
	unsigned short data;
	intc_reg_read(data, addr);
	intc_reg_write(addr, data | mask);
}

static void intc_reg_clear(unsigned short addr, unsigned short mask)
{
	unsigned short data;
	intc_reg_read(data, addr);
	intc_reg_write(addr, data & ~mask);
}

static int intc_enable(int intc_line, int enable)
{
	if(enable)
		intc_reg_set(INTC_EN_OFFSET, 1 << intc_line);
	else
		intc_reg_clear(INTC_EN_OFFSET, 1 << intc_line);

	return 0;
}

static void intc_addr_data_free(int ii)
{
	if(fid[ii].addr) {
		kfree(fid[ii].addr);
		fid[ii].addr = NULL;
	}
	if(fid[ii].data) {
		kfree(fid[ii].data);
		fid[ii].data = NULL;
	}
	fid[ii].rcnt = 0;
}

static int intc_kmalloc(int ii, int bytes)
{
	int k;

	if(!(fid[ii].addr = (unsigned short *)kmalloc(bytes, GFP_ATOMIC))) {
		printk(KERN_ERR "<%s> kmalloc failed\n", DEVICE_NAME);
		return -1;
	}

	if(!(fid[ii].data = (unsigned short *)kmalloc(bytes, GFP_ATOMIC))) {
		printk(KERN_ERR "<%s> kmalloc failed\n", DEVICE_NAME);
		kfree(fid[ii].addr);
		fid[ii].addr = NULL;
		return -1;
	}

	// initialize data
	for(k = 0; k < bytes; k++)
		fid[ii].data[k] = INTC_DATA_INVALID;

	fid[ii].rcnt = bytes / sizeof(unsigned short);

	return bytes;
}

static ssize_t intc_write(struct file *f, const char __user *buf, size_t bytes, loff_t * ppos)
{
	int ii, k;

	ii = iminor(f->f_inode);

	printk(KERN_DEBUG "<%s> file: write() %d\n", DEVICE_NAME, ii);

	// free if necessary
	intc_addr_data_free(ii);

	if (intc_kmalloc(ii, bytes) < 0)
	return -ENOMEM;

	if (copy_from_user((void *)fid[ii].addr, (const void __user *)buf, bytes))
	{
		intc_addr_data_free(ii);
		return -EFAULT;
	}
	// mask addresses
	for (k=0; k<bytes/sizeof(unsigned short); k++)
	fid[ii].addr[k] &= INTC_ADDR_MASK;

	return bytes;
}

static ssize_t intc_read(struct file *f, char __user * buf, size_t bytes, loff_t * off)
{
	int ii;

	ii = iminor(f->f_inode);

	printk(KERN_DEBUG "<%s> file: read()  %d\n", DEVICE_NAME, ii);

	if (f->f_flags & O_NONBLOCK)
	{
		if (!fid[ii].valid)
		return -EAGAIN;
	}
	else
	{
		int ret = fid[ii].timeout;

		// wake me up once interrupt received
		wait_event_interruptible_timeout(wq, fid[ii].valid, ret);
		if (!fid[ii].valid)// timeout
		return -ETIME;
		if (ret == -ERESTARTSYS && !fid[ii].valid)// unconditionally restart the system call
		{
			printk(KERN_DEBUG "<%s> file: read()  %d ERESTARTSYS\n", DEVICE_NAME, ii);
			return ret;
		}
	}

	// copy data if any
	if (!buf || !fid[ii].rcnt)
	{
		bytes = 0;
	}
	else
	{
		if (bytes > fid[ii].rcnt * sizeof(unsigned short))
		bytes = fid[ii].rcnt * sizeof(unsigned short);
		if (copy_to_user((unsigned int *)buf, fid[ii].data, bytes))
		{
			printk(KERN_ERR "<%s> file: read()  %d EFAULT\n", DEVICE_NAME, ii);
			return -EFAULT;
		}
	}

	// clear valid once I'm awakened & have copied data to user
	fid[ii].valid = false;

	return bytes;
}

static irqreturn_t intc_isr(int num, void *dev_id)
{
	unsigned short pending;
	unsigned long flags;
	int ii, k;

	spin_lock_irqsave(&ilock, flags);

	intc_reg_write(INTC_MASK_OFFSET, 1); // mask all interrupts
	intc_reg_read(pending, INTC_PEND_OFFSET); // determine pending IRQ

	// address FPGA IRQ interrupts, right to left
	for(ii = 0; ii < INTC_IRQ_COUNT; ii++) {
		if((pending & (1 << ii)) == 0)
			continue;
		//printk(KERN_DEBUG "<%s> isr() %d\n", DEVICE_NAME, ii);
		// read registers
		for(k = 0; k < fid[ii].rcnt; k++)
			fid[ii].data[k] = fpga_reg_read(fid[ii].addr[k])
		;

		fid[ii].count++;

		// wake up read()
		fid[ii].valid = true;
		wake_up_interruptible_sync(&wq);
	}
	intc_reg_write(INTC_CLR_OFFSET, pending); // clear interrupts
	intc_reg_write(INTC_MASK_OFFSET, 0); // unmask

	spin_unlock_irqrestore(&ilock, flags);

	return IRQ_HANDLED;
}

static long intc_irq_init(int num)
{
	int ii;

	if(irqnum)
		free_irq(irqnum, NULL); // free previously requested IRQ

	irqnum = num;
	for(ii = 0; ii < INTC_IRQ_COUNT; ii++) {
		fid[ii].valid = false;
		intc_addr_data_free(ii);
	}

	if(request_irq(irqnum, intc_isr, 0, DEVICE_NAME, NULL)) {
		printk(KERN_ERR "<%s> unable to register IRQ %d\n", DEVICE_NAME, irqnum);
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

	ii = iminor(f->f_inode);
	switch(request) {
	case INTC_INT_COUNT: {
		printk(KERN_INFO "<%s> file: ioctl() %d, interrupt count:%ld\n", DEVICE_NAME, ii, fid[ii].count);
		ret = fid[ii].count;
		break;
	}
	case INTC_ENABLE: {
		int enable;
		if(copy_from_user((void *)&enable, (const void __user *)arg, sizeof(int)))
			return -EFAULT;

		printk(KERN_INFO "<%s> file: ioctl() %d, enable:%d\n", DEVICE_NAME, ii, enable);
		ret = intc_enable(ii, enable);
		break;
	}
	case INTC_TIMEOUT: {
		int milliseconds;
		if(copy_from_user((void *)&milliseconds, (const void __user *)arg, sizeof(int)))
			return -EFAULT;

		printk(KERN_INFO "<%s> file: ioctl() %d, timeout:%d\n", DEVICE_NAME, ii, milliseconds);
		fid[ii].timeout = (milliseconds * HZ) / 1000;
		break;
	}
	default:
		printk(KERN_ERR "<%s> file: ioctl() %d, unrecognized request %d\n", DEVICE_NAME, ii, request);
		ret = -EINVAL;
		break;
	}

	if(copy_to_user((unsigned int*)arg, &ret, sizeof(int)))
		return -EFAULT;

	return 0;
}

static int intc_open(struct inode *inode, struct file *f)
{
	int ii;
	ii = iminor(f->f_inode);

	if(fid[ii].open)
		return -EBUSY;

	if(open_files == 0) {
		int res = intc_irq_init(irq);
		if(res != 0)
			return res;
	}

	open_files++;
	printk(KERN_DEBUG "<%s> file: open()  %d (%d opened)\n", DEVICE_NAME, ii, open_files);

	fid[ii].open = true;

	return intc_enable(ii, 1);
}

static int intc_close(struct inode *inode, struct file *f)
{
	int ii;
	ii = iminor(f->f_inode);

	intc_enable(ii, 0);
	open_files--;
	fid[ii].open = false;
	if(open_files == 0) {
		free_irq(irqnum, NULL);
		irqnum = 0;
	}

	printk(KERN_DEBUG "<%s> file: close() %d (%d opened)\n", DEVICE_NAME, ii, open_files);

	return 0;
}

static struct file_operations fops = {.owner = THIS_MODULE, .open = intc_open, .release = intc_close, .write =
		intc_write, .read = intc_read, .unlocked_ioctl = intc_ioctl};

ssize_t intc_name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	if(fid[MINOR(dev->devt)].name)
		return snprintf(buf, PAGE_SIZE, "%s\n", fid[MINOR(dev->devt)].name);
	else
		return 0;
}

ssize_t intc_count_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%ld\n", fid[MINOR(dev->devt)].count);
}

ssize_t intc_timeout_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	s32 milliseconds;
	milliseconds = fid[MINOR(dev->devt)].timeout * 1000 / HZ;
	return snprintf(buf, PAGE_SIZE, "%d\n", milliseconds);
}

ssize_t intc_timeout_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	char *end;
	s32 timeout;
	unsigned long new = simple_strtoul(buf, &end, 0);
	if(end == buf)
		return -EINVAL;

	timeout = (new * HZ) / 1000;
	fid[MINOR(dev->devt)].timeout = timeout;
	// Always return full write size even if we didn't consume all
	return size;
}

ssize_t intc_enable_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int enabled = 0;
	unsigned short res;
	intc_reg_read(res, INTC_EN_OFFSET)
	&(1 << MINOR(dev->devt));
	if(res)
		enabled = 1;
	return snprintf(buf, PAGE_SIZE, "%d\n", enabled);
}

ssize_t intc_enable_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	char *end;
	unsigned long enable = simple_strtoul(buf, &end, 0);
	if(end == buf || (enable != 1 && enable != 0))
		return -EINVAL;

	intc_enable(MINOR(dev->devt), enable);
	// Always return full write size even if we didn't consume all
	return size;
}

//TODO: This should be a debugfs entry instead.
ssize_t intc_set_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	intc_reg_write(INTC_SET_OFFSET, 0);
	intc_reg_set(INTC_SET_OFFSET, 1 << MINOR(dev->devt));
	// Always return full write size even if we didn't consume all
	return size;
}

static DEVICE_ATTR(name, S_IRUGO, intc_name_show, NULL);
static DEVICE_ATTR(count, S_IRUGO, intc_count_show, NULL);
static DEVICE_ATTR(timeout_ms, (S_IRUGO | S_IWUSR | S_IWGRP), intc_timeout_show, intc_timeout_store);
static DEVICE_ATTR(enable, (S_IRUGO | S_IWUSR | S_IWGRP), intc_enable_show, intc_enable_store);
static DEVICE_ATTR(set, S_IWUSR, NULL, intc_set_store);

static struct attribute *attrs[] = {
		&dev_attr_name.attr,
		&dev_attr_count.attr,
		&dev_attr_timeout_ms.attr,
		&dev_attr_enable.attr,
		&dev_attr_set.attr,
		NULL, };

static struct attribute_group attr_group = {.attrs = attrs, };

static const struct attribute_group *attr_groups[] = {&attr_group, NULL, };

static int intc_chrdev_init(void)
{
	int ii;
	if(alloc_chrdev_region(&dev, 0, INTC_IRQ_COUNT, DEVICE_NAME) < 0) {
		printk(KERN_ERR "<%s> init: alloc_chrdev_region failed\n", DEVICE_NAME);
		return -1;
	}
	if((cl = class_create(THIS_MODULE, DEVICE_NAME)) == NULL) {
		printk(KERN_ERR "<%s> init: class_create failed\n", DEVICE_NAME);
		unregister_chrdev_region(dev, INTC_IRQ_COUNT);
		return -1;
	}

	cl->dev_groups = attr_groups;

	for(ii = 0; ii < INTC_IRQ_COUNT; ii++) {
		if(device_create(cl, NULL, MKDEV(MAJOR(dev), MINOR(dev) + ii), NULL, "intc%d", ii) == NULL) {
			printk(KERN_ERR "<%s> init: device_create failed\n", DEVICE_NAME);
			class_destroy(cl);
			unregister_chrdev_region(dev, INTC_IRQ_COUNT);
			return -1;
		}
	}

	cdev_init(&cdev, &fops);
	if(cdev_add(&cdev, dev, INTC_IRQ_COUNT) == -1) {
		printk(KERN_ERR "<%s> init: cdev_add failed\n", DEVICE_NAME);
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
	for(ii = 0; ii < INTC_IRQ_COUNT; ii++) {
		intc_addr_data_free(ii);
		device_destroy(cl, MKDEV(MAJOR(dev), MINOR(dev) + ii));
	}
	class_destroy(cl);
	unregister_chrdev_region(dev, INTC_IRQ_COUNT);
	printk(KERN_DEBUG "<%s> exit: unregistered\n", DEVICE_NAME);

	return 0;
}

static int intc_map_vfpga(struct resource *res)
{
	vfpga = ioremap_nocache(res->start & 0xffff0000, FPGA_REGS_SIZE);
	if(!vfpga) {
		printk(KERN_ERR "<%s> ioremap_nocache failed\n", DEVICE_NAME);
		return -ENOMEM;
	}
	return 0;
}

static int intc_of_probe(struct platform_device *ofdev)
{
	int ret, ii;
	struct device_node *child;
	struct resource *res;
	resource_size_t size;

	dev_info(&ofdev->dev, "%s version: %s (%s)\n", "IMSAR intc driver", GIT_DESCRIBE, BUILD_DATE);

	res = platform_get_resource(ofdev, IORESOURCE_IRQ, 0);
	if(!res) {
		printk(KERN_ERR "<%s> probe: could not get platform IRQ resource\n", DEVICE_NAME);
		return -1;
	}

	irq = res->start;
	printk(KERN_INFO "<%s> probe: IRQ read from DTS entry as %d\n", DEVICE_NAME, irq);
	res = platform_get_resource(ofdev, IORESOURCE_MEM, 0);
	size = resource_size(res);
#if DEBUG
	printk(KERN_INFO "<%s> probe: register physical base address = %x\n", DEVICE_NAME, res->start);
#endif

	vbase = of_iomap(ofdev->dev.of_node, 0);
	if(!vbase) {
		dev_err(&ofdev->dev, "of_iomap failed for resource %pR\n", res);
		return -ENOMEM;
	}

	// for register read backs
	if(intc_map_vfpga(res) != 0)
		return -ENOMEM;

	spin_lock_init(&ilock);

	// register the character device
	if((ret = intc_chrdev_init()) != 0)
		return ret;
	for(ii = 0; ii < INTC_IRQ_COUNT; ii++) {
		// initialize interrupts
		fid[ii].valid = false;
		fid[ii].open = false;
		fid[ii].rcnt = 0;
		fid[ii].addr = NULL;
		fid[ii].data = NULL;
		fid[ii].name = NULL;
		fid[ii].count = 0;

		// disable timeouts
		fid[ii].timeout = MAX_SCHEDULE_TIMEOUT;
	}

	for_each_child_of_node(ofdev->dev.of_node, child)
	{
		u32 index;
		s32 milliseconds;

		ret = of_property_read_u32(child, "reg", &index);
		if(ret < 0) {
			dev_info(&ofdev->dev, "no property reg for child of FPGA interrupt controller\n");
		} else {
			fid[index].name = child->name;
		printk(KERN_INFO "interrupt #%d = %s\n", index, child->name);
	}

	ret = of_property_read_u32(child, "timeout_ms", &milliseconds);
	if(ret < 0) {
		dev_info(&ofdev->dev, "no property timeout for child of FPGA interrupt controller\n");
	} else {
		printk(KERN_INFO "interrupt #%d timeout = %d\n", index, milliseconds);
		fid[index].timeout = (milliseconds * HZ) / 1000;
	}

}

intc_reset();

printk(KERN_INFO "<%s> init: registered\n", DEVICE_NAME);

return 0;
}

static const struct of_device_id intc_of_match[] = {{.compatible = "imsar,intc", }, { /* end of list */}, };
MODULE_DEVICE_TABLE( of, intc_of_match);

static struct platform_driver intc_of_driver = {.probe = intc_of_probe, .remove = intc_of_remove, .driver = {.name =
DEVICE_NAME, .owner = THIS_MODULE, .of_match_table = intc_of_match, }, };
module_platform_driver( intc_of_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Joseph Hancock <joseph.hancock@imsar.com> & Derrick Gibelyou <derrick.gibelyou@imsar.com>");
MODULE_DESCRIPTION("Driver for ImSAR FPGA interrupt controller");
MODULE_VERSION(GIT_DESCRIBE);
