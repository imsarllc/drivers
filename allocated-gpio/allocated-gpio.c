/*
 * allocated-gpio.c
 *
 * Created on: Apr 2, 2018
 * Author: Derrick Gibelyou
 */
#include "version.h"

#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <dt-bindings/gpio/gpio.h>
#include <asm-generic/errno.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/of_device.h>
#include <linux/err.h>
#include <linux/module.h>

#define DEVICE_NAME "allocated-gpio"

struct gpio_attribute
{
	struct device_attribute n;
	s32 gpio;
	s32 flags;
};

struct gpio_driver_data
{
	struct gpio_attribute* attr_array;
	struct attribute** attr_list;
	struct attribute_group reg_attr_group;
	int num_attrs;
};


ssize_t gpio_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct gpio_driver_data *data = (struct gpio_driver_data*) dev->platform_data;
	int ii = 0;
	for (ii = 0; ii < data->num_attrs; ii++)
	{
		if (attr == &(data->attr_array[ii].n) )
		{
			int ret;
			//printk(KERN_DEBUG "Address match for %s\n", data->attr_array[ii].n.attr.name);
			ret = gpio_get_value_cansleep(data->attr_array[ii].gpio);
			if (data->attr_array[ii].flags & GPIOF_ACTIVE_LOW)
				ret = !ret;
			return snprintf(buf, PAGE_SIZE, "%d\n", ret);
		}
	}
	return snprintf(buf, PAGE_SIZE, "error\n");
}

ssize_t gpio_state_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct gpio_driver_data *data = (struct gpio_driver_data*) dev->platform_data;
	int ii = 0;
	if (buf[0] != '0' && buf[0] != '1' && strncasecmp(buf, "z", 1))
	{
		printk(KERN_ERR "Invalid GPIO value: '%s'\n", buf);
		return -EINVAL;
	}

	printk(KERN_DEBUG "Setting output %s to %c\n", attr->attr.name, buf[0]);
	for (ii = 0; ii < data->num_attrs; ii++)
	{
		if (attr == &(data->attr_array[ii].n) )
		{

			if (0 == strncasecmp(buf, "z", 1))
			{
				gpio_direction_input(data->attr_array[ii].gpio);
			}
			else
			{
				int value = buf[0]-'0';
				if (data->attr_array[ii].flags & GPIOF_ACTIVE_LOW)
					value = !value;
				gpio_direction_output(data->attr_array[ii].gpio, value);
			}
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
	struct gpio_driver_data *data;

	int status;
	size_t attr_size;
	size_t attr_list_size;
	u32 num_attrs = of_get_child_count(np);

	data = (struct gpio_driver_data*)kzalloc(sizeof(struct gpio_driver_data), GFP_KERNEL);
	if (PTR_ERR_OR_ZERO(data))
	{
		goto error;
	}
	pdev->dev.platform_data = data;

	printk(KERN_DEBUG "Creating %d attributes for %s\n", num_attrs, np->name);

	attr_size = num_attrs * sizeof(struct gpio_attribute);
	data->attr_array = (struct gpio_attribute*)kzalloc(attr_size, GFP_KERNEL);
	if (PTR_ERR_OR_ZERO(data->attr_array))
	{
		goto array_error;
	}

	attr_list_size = (num_attrs + 1) * sizeof(struct attribute*);
	data->attr_list = (struct attribute**)kzalloc(attr_size, GFP_KERNEL);
	if (PTR_ERR_OR_ZERO(data->attr_list))
	{
		goto list_error;
	}

	num_attrs = 0;
	for_each_child_of_node(np, child)
	{
		u32 flags = 0;
		enum of_gpio_flags of_flags = 0;
		s32 gpio = of_get_gpio_flags(child, 0, &of_flags);
		if (gpio == -EPROBE_DEFER) {
			dev_info(&pdev->dev, "GPIO %s not available yet.  Try Again?\n", child->name);
			continue;
		}
		if (gpio < 0) {
			dev_info(&pdev->dev, "no property gpio for child of allocated-gpio\n");
			continue;
		}
		//printk(KERN_INFO "GPIO #%d = %s(%d)\n", gpio, child->name, flags);

		data->attr_array[num_attrs].n.attr.name = child->name;
		data->attr_array[num_attrs].n.attr.mode = S_IRUGO;
		data->attr_array[num_attrs].n.show = gpio_state_show;
		data->attr_array[num_attrs].gpio = gpio;

		if (of_flags & GPIO_ACTIVE_LOW)
			flags = GPIOF_ACTIVE_LOW;

		if (of_property_read_bool(child, "output-low"))
			flags |= GPIOF_OUT_INIT_LOW | GPIOF_EXPORT_DIR_FIXED;
		else if (of_property_read_bool(child, "output-high"))
			flags |= GPIOF_OUT_INIT_HIGH | GPIOF_EXPORT_DIR_FIXED;
		else if (of_property_read_bool(child, "input"))
			flags |= GPIOF_IN | GPIOF_EXPORT_DIR_FIXED;
		else
			flags |= GPIOF_IN | GPIOF_EXPORT_DIR_CHANGEABLE;
		data->attr_array[num_attrs].flags = flags;

		if (! of_property_read_bool(child, "input") )
		{
			data->attr_array[num_attrs].n.store = gpio_state_store;
			data->attr_array[num_attrs].n.attr.mode = (S_IWUSR|S_IWGRP) | S_IRUGO;
			if(gpio_direction_input(data->attr_array[num_attrs].gpio)) {
				dev_info(&pdev->dev, "Unable to set GPIO to input\n");
			}
		}

		printk(KERN_INFO "GPIO #%d = %s(%d)\n", gpio, child->name, flags);
		status = gpio_request_one(gpio, flags, child->name);
		if (status)
		{
			dev_info(&pdev->dev, "Unable to request GPIO: %d(%s)", gpio, child->name);
			continue;
		}
		gpio_export_link(&pdev->dev, child->name, gpio);
		data->attr_list[num_attrs] = &data->attr_array[num_attrs].n.attr;
		num_attrs++;
	}
	data->num_attrs = num_attrs;
	data->reg_attr_group.attrs = data->attr_list;
	data->reg_attr_group.name ="io";
	status = sysfs_create_group(&pdev->dev.kobj, &data->reg_attr_group);
	if (status)
		printk(KERN_ERR "Failed to create pin attributes: %d\n", status);
	return;

list_error:
	data->attr_list = 0;
	kfree(data->attr_array);
array_error:
	data->attr_array = 0;
error:
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
	printk(KERN_ERR "Probing allocated gpio\n");

	dev_info(&pdev->dev, "%s version: %s (%s)\n", "IMSAR gpio driver", GIT_DESCRIBE, BUILD_DATE);

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
	int ii;
	struct gpio_driver_data *data = (struct gpio_driver_data*) pdev->dev.platform_data;

	for (ii = 0; ii < data->num_attrs; ii++)
	{
		s32 gpio;
		gpio = data->attr_array[ii].gpio;
		dev_info(&pdev->dev, "removing GPIO = %s:%d", data->attr_array[ii].n.attr.name, gpio);
		if (gpio >= 0)
		{
				gpio_unexport(gpio);
				sysfs_remove_link(&pdev->dev.kobj, data->attr_array[ii].n.attr.name);
				gpio_free(gpio);
		}
	}

	sysfs_remove_group(&pdev->dev.kobj, &data->reg_attr_group);
	if (data->attr_list)
		kfree(data->attr_list);
	if (data->attr_array)
		kfree(data->attr_array);

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
