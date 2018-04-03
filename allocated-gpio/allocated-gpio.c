/*
 * allocated-gpio.c
 *
 *  Created on: Apr 2, 2018
 *      Author: Derrick Gibelyou
 */
#include "version.h"

#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <asm-generic/errno.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/of_device.h>

#define DEVICE_NAME "allocated-gpio"

struct gpio_attribute
{
	struct device_attribute n;
	u32 gpio;
};

static struct gpio_attribute* attr_array = 0;
static struct attribute** attr_list = 0;
static struct attribute_group* reg_attr_group = 0;
static int num_attrs = 0;

ssize_t gpio_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int ii = 0;
	for (ii = 0; ii < num_attrs; ii++)
	{
		if (attr == &(attr_array[ii].n) )
		{
			int ret;
			printk(KERN_DEBUG "Address match for %s\n", attr_array[ii].n.attr.name);
			ret = gpio_get_value_cansleep(attr_array[ii].gpio);
			return snprintf(buf, PAGE_SIZE, "%d\n", ret);
		}
	}
	return snprintf(buf, PAGE_SIZE, "error\n");
}

ssize_t gpio_state_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	int ii = 0;
	int value = buf[0]-'0';
	if (value != 0 && value != 1)
	{
		printk(KERN_ERR "Invalid GPIO value: '%s'\n", buf);
		return -EINVAL;
	}

	printk(KERN_DEBUG "Setting output %s to %c\n", attr->attr.name, buf[0]);
	for (ii = 0; ii < num_attrs; ii++)
	{
		if (attr == &(attr_array[ii].n) )
		{
			printk(KERN_DEBUG "Address match for %s\n", attr_array[ii].n.attr.name);
			gpio_direction_output(attr_array[ii].gpio, value);
			return size;
		}
	}
	return -EIO;
}

static struct of_device_id allocated_gpio_of_match[] = {
		{.compatible = "allocated-gpio"},
		{ /* end of table*/}
};
MODULE_DEVICE_TABLE(of, allocated_gpio_of_match);

static void create_pin_attrs(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *child;

	int status;
	size_t attr_size;
	size_t attr_list_size;

	num_attrs = of_get_child_count(np);

	printk(KERN_DEBUG "Creating %d attributes for %s\n", num_attrs, np->name);

	attr_size = num_attrs * sizeof(struct gpio_attribute);
	attr_array = (struct gpio_attribute*)kzalloc(attr_size, GFP_KERNEL);
	if(PTR_ERR_OR_ZERO(attr_array))
	{
		goto array_error;
	}

	attr_list_size = (num_attrs + 1) * sizeof(struct attribute*);
	attr_list = (struct attribute**)kzalloc(attr_size, GFP_KERNEL);
	if(PTR_ERR_OR_ZERO(attr_list))
	{
		goto list_error;
	}

	reg_attr_group = (struct attribute_group*) kzalloc(sizeof(struct attribute_group),GFP_KERNEL);
	if(PTR_ERR_OR_ZERO(reg_attr_group))
	{
		goto group_error;
	}

	num_attrs = 0;
	for_each_child_of_node(np, child)
	{
		u32 gpio = of_get_gpio(child, 0);
		if(gpio < 0) {
			dev_info(&pdev->dev, "no property gpio for child of FPGA interrupt controller\n");
			continue;
		}
		status = gpio_request(gpio, child->name);
		if(status)
		{
			dev_info(&pdev->dev, "Unable to request GPIO: %d(%s)", gpio, child->name);
			continue;
		}
		printk(KERN_INFO "interrupt #%d = %s\n", gpio, child->name);
		attr_array[num_attrs].n.attr.name = child->name;
		attr_array[num_attrs].n.attr.mode = S_IRUGO;
		attr_array[num_attrs].n.show = gpio_state_show;
		//TODO: check dt param for input/output
		if (1)
		{
			attr_array[num_attrs].n.store = gpio_state_store;
			attr_array[num_attrs].n.attr.mode = S_IWUGO | S_IRUGO;
		}
		attr_array[num_attrs].gpio = gpio;
		attr_list[num_attrs] = &attr_array[num_attrs].n.attr;
		num_attrs++;
	}

	reg_attr_group->attrs = attr_list;
	reg_attr_group->name ="io";
	status = sysfs_create_group(&pdev->dev.kobj, reg_attr_group);
	if (status)
		printk(KERN_ERR "Failed to create pin attributes: %d\n", status);
	return;

group_error:
	reg_attr_group = 0;
list_error:
	kfree(attr_list);
	attr_list = 0;
array_error:
	kfree(attr_array);
	attr_array = 0;

	printk(KERN_ERR "Unable to allocate register attributes\n");
}


/**
 * allocated_gpio_probe - Driver probe function
 * @pdev: Pointer to the platform_device structure
 *
 * Returns '0' on success and failure value on error
 */

static int allocated_gpio_probe(struct platform_device *pdev)
{
	printk(KERN_ERR "Probing power gpio\n");

	dev_info(&pdev->dev, "%s version: %s (%s)\n", "IMSAR intc driver", GIT_DESCRIBE, BUILD_DATE);

	create_pin_attrs(pdev);

	dev_info(&pdev->dev, "Probed IMSAR allocated_gpio\n");

	return 0;
}

/**
 * allocated_gpio_remove - Driver remove function
 * @pdev: Pointer to the platform_device structure
 *
 * Always returns '0'
 */
static int allocated_gpio_remove(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *child;

	if (reg_attr_group)
		sysfs_remove_group(&pdev->dev.kobj, reg_attr_group);
	if (reg_attr_group)
		kfree(reg_attr_group);
	if (attr_list)
		kfree(attr_list);
	if (attr_array)
		kfree(attr_array);

	for_each_child_of_node(np, child)
	{
		u32 gpio;
		gpio = of_get_gpio(child, 0);
		//FIXME: If somebody else requested this GPIO, we will free it for them :(
		gpio_free(gpio);
	}

	return 0;
}


static struct platform_driver allocated_gpio_driver = {
		.driver = {
				.name = DEVICE_NAME,
				.of_match_table = allocated_gpio_of_match,
		},
		.probe = allocated_gpio_probe,
		.remove = allocated_gpio_remove,
};
module_platform_driver(allocated_gpio_driver);

MODULE_AUTHOR("IMSAR LLC");
MODULE_DESCRIPTION("GPIO to sysfs node device wrapper");
MODULE_LICENSE("GPL v2");
MODULE_VERSION(GIT_DESCRIBE);