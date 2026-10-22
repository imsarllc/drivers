#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>

#include "version.h"

/* Defines for the character device */
#define DRIVER_NAME "named-gpio"
#define DEVICE_NAME "named-gpio"
#define CLASS_NAME "imsar_gpio"

/* Global, shared class pointer for all devices using this driver */
static struct class *imsar_named_gpio_class;

enum imsar_named_gpio_direction
{
	IMSAR_NAMED_GPIO_DIRECTION_INPUT,
	IMSAR_NAMED_GPIO_DIRECTION_OUTPUT,
};

/* A structure to hold our device-specific data (allocated per instance) */
struct gpio_dev
{
	struct device *dev; // Parent platform device pointer for logging context

	// character device
	struct cdev cdev;           // Character device structure
	dev_t dev_num;              // Device number (major/minor)
	struct device *char_device; // Pointer to the created /dev device file

	// gpio
	struct gpio_desc *gpio_desc; // GPIO descriptor handle (devm_ managed)

	// metadata
	const char *name; // Custom name/label read from DT
	enum imsar_named_gpio_direction direction;
	int initial_value;
};

/*******************************************************************************
 * Sysfs Attribute Functions (for udev integration)
 ******************************************************************************/

/**
 * @brief Show the custom GPIO label read from the Device Tree.
 */
static ssize_t named_gpio_name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	// Retrieve instance data from the device pointer
	struct gpio_dev *gdev = dev_get_drvdata(dev);
	if (!gdev)
	{
		return 0;
	}
	return snprintf(buf, PAGE_SIZE, "%s\n", gdev->name);
}

// Define the read-only attributes
static DEVICE_ATTR(name, S_IRUGO, named_gpio_name_show, NULL);

// Group the attributes
static struct attribute *named_gpio_attrs[] = {
    &dev_attr_name.attr,
    NULL,
};

static const struct attribute_group named_gpio_group = {
    .attrs = named_gpio_attrs,
};

static const struct attribute_group *named_gpio_attr_groups[] = {
    &named_gpio_group,
    NULL,
};

/*******************************************************************************
 * File Operations Helpers
 ******************************************************************************/

/**
 * @brief Opens the character device file.
 * @param inode Pointer to the inode structure.
 * @param filp Pointer to the file structure.
 * @return 0 on success, negative error code on failure.
 */
static int named_gpio_open(struct inode *inode, struct file *filp)
{
	// Retrieve instance data from the inode's i_cdev, which points to the cdev struct.
	// Here we use container_of to retrieve our instance data.
	struct gpio_dev *gdev = container_of(inode->i_cdev, struct gpio_dev, cdev);

	// Increment module usage count to prevent unloading while device is open
	if (!try_module_get(THIS_MODULE))
	{
		dev_err(gdev->dev, "Failed to get module reference (in use)\n");
		return -ENODEV;
	}

	// Store the instance data pointer in filp->private_data for read/write access
	filp->private_data = gdev;

	dev_dbg(gdev->dev, "Device opened\n");
	return 0;
}

/**
 * @brief Closes the character device file.
 * @param inode Pointer to the inode structure.
 * @param filp Pointer to the file structure.
 * @return 0 on success.
 */
static int named_gpio_release(struct inode *inode, struct file *filp)
{
	struct gpio_dev *gdev = filp->private_data;

	// Decrement module usage count
	module_put(THIS_MODULE);

	dev_dbg(gdev->dev, "Device released\n");
	return 0;
}

/**
 * @brief Reads the current state of the GPIO pin.
 * @param filp Pointer to the file structure.
 * @param buf User space buffer to copy data to.
 * @param count Number of bytes to read (should be at least 1).
 * @param off Pointer to the file position offset.
 * @return Number of bytes read, or negative error code.
 */
static ssize_t named_gpio_read(struct file *filp, char __user *buf, size_t count, loff_t *off)
{
	struct gpio_dev *gdev = filp->private_data;
	int value;
	char buffer[10] = {0};
	int len;

	if (*off > 0)
	{
		return 0; // End of file
	}

	// Read the actual GPIO pin value (0 or 1)
	value = gpiod_get_value(gdev->gpio_desc);
	if (value < 0)
	{
		dev_err(gdev->dev, "Failed to read GPIO value\n");
		return value;
	}
	dev_dbg(gdev->dev, "Read value: %d\n", value);

	// Convert the integer value (0 or 1) to an ASCII character ('0' or '1')

	len = snprintf(buffer, sizeof(buffer), "%d\n", value);

	// Copy the result to user space
	if (copy_to_user(buf, buffer, len))
	{
		return -EFAULT;
	}

	*off += len; // Advance file pointer
	return len;
}

/**
 * @brief Writes a new state (0 or 1) to the GPIO pin.
 * @param filp Pointer to the file structure.
 * @param buf User space buffer to copy data from.
 * @param count Number of bytes to write (should be at least 1).
 * @param off Pointer to the file position offset.
 * @return Number of bytes written, or negative error code.
 */
static ssize_t named_gpio_write(struct file *filp, const char __user *buf, size_t count, loff_t *off)
{
	struct gpio_dev *gdev = filp->private_data;
	char buffer[10] = {0};
	int value;
	char *end;

	if (count == 0)
	{
		return 0;
	}

	if (count > sizeof(buffer))
	{
		count = sizeof(buffer);
	}

	// Copy the first character from user space
	if (copy_from_user(&buffer, buf, count))
	{
		return -EFAULT;
	}

	value = simple_strtoul(buffer, &end, 0);
	value = (value != 0); // force to 0 or 1

	// // Write the value to the GPIO pin
	gpiod_set_value(gdev->gpio_desc, value);

	dev_dbg(gdev->dev, "Wrote value: %d\n", value);

	return count;
}

// Define the file operations structure, now including open and release
static const struct file_operations gpio_fops = {
    .owner = THIS_MODULE,
    .open = named_gpio_open,
    .release = named_gpio_release,
    .read = named_gpio_read,
    .write = named_gpio_write,
};

/*******************************************************************************
 * Platform Driver Core Functions (probe/remove)
 ******************************************************************************/

/**
 * @brief Driver probe function (called when device node is matched).
 */
static int named_gpio_probe(struct platform_device *pdev)
{
	int ret;
	struct device *device_file;
	struct gpio_dev *gdev;
	const char *direction;
	struct device *dev = &pdev->dev;

	dev_dbg(dev, "Probe function called\n");

	// Allocate instance-specific memory (devm_ cleanup is implicit on error/remove)
	gdev = devm_kzalloc(dev, sizeof(*gdev), GFP_KERNEL);
	if (!gdev)
	{
		return -ENOMEM;
	}

	// Store the driver data pointer in the platform device structure
	platform_set_drvdata(pdev, gdev);
	gdev->dev = dev;

	// Get the GPIO Descriptor from Device Tree using devm_
	gdev->gpio_desc = devm_gpiod_get_from_of_node(dev, dev->of_node, "gpio", 0, GPIOD_ASIS, DRIVER_NAME);

	if (IS_ERR(gdev->gpio_desc))
	{
		ret = PTR_ERR(gdev->gpio_desc);
		dev_err_probe(dev, "Failed to get GPIO from DT (error %d)\n", ret);
		return ret;
	}

	// Get GPIO Label from Device Tree
	ret = of_property_read_string(dev->of_node, "imsar,name", &gdev->name);
	if (ret)
	{
		dev_err(dev, "Missing required 'imsar,name' property\n");
		return -EINVAL;
	}

	// Get the direction
	ret = of_property_read_string(dev->of_node, "imsar,direction", &direction);
	if (ret)
	{
		dev_err(dev, "Missing required 'imsar,direction' property\n");
		return -EINVAL;
	}

	if (strcmp(direction, "input") == 0)
	{
		gdev->direction = IMSAR_NAMED_GPIO_DIRECTION_INPUT;
	}
	else if (strcmp(direction, "output") == 0)
	{
		gdev->direction = IMSAR_NAMED_GPIO_DIRECTION_OUTPUT;
	}
	else
	{
		dev_err(dev, "Unrecognized imsar,direction: %s (options: \"input\" or \"output\")", direction);
		return -EINVAL;
	}


	// Set pin direction
	switch (gdev->direction)
	{
	case IMSAR_NAMED_GPIO_DIRECTION_INPUT:
		ret = gpiod_direction_input(gdev->gpio_desc);
		break;
	case IMSAR_NAMED_GPIO_DIRECTION_OUTPUT:
		// Get value
		ret = of_property_read_u32(dev->of_node, "imsar,initial-value", &gdev->initial_value);
		if (ret)
		{
			dev_warn(dev, "No 'imsar,initial-value' property: assuming 0\n");
			gdev->initial_value = 0;
		}

		ret = gpiod_direction_output(gdev->gpio_desc, gdev->initial_value);
		break;
	default:
		dev_err(dev, "Bad direction: %d\n", gdev->direction);
		return -EINVAL;
	};

	if (ret)
	{
		dev_err(dev, "Failed to set GPIO direction (error %d)\n", ret);
		return ret;
	}

	// Allocate a Major/Minor number for the character device
	// We request one minor number starting from 0.
	ret = alloc_chrdev_region(&gdev->dev_num, 0, 1, DEVICE_NAME);
	if (ret < 0)
	{
		dev_err(dev, "Failed to allocate char device region\n");
		return ret;
	}

	// Initialize and add the CDEV
	cdev_init(&gdev->cdev, &gpio_fops);
	gdev->cdev.owner = THIS_MODULE;
	ret = cdev_add(&gdev->cdev, gdev->dev_num, 1);
	if (ret < 0)
	{
		dev_err(dev, "Failed to add cdev\n");
		goto err_unregister_chrdev;
	}

	// Create the device file in /dev using devm_ and the shared class
	device_file = device_create(imsar_named_gpio_class, gdev->dev, gdev->dev_num, gdev, "named_gpio_%s", gdev->name);
	if (IS_ERR(device_file))
	{
		dev_err(dev, "Failed to create device file\n");
		ret = PTR_ERR(device_file);
		goto err_del_cdev;
	}
	gdev->char_device = device_file;

	dev_info(dev, "Created named GPIO %s\n", gdev->name);

	return 0; // Success

err_del_cdev:
	cdev_del(&gdev->cdev);
err_unregister_chrdev:
	unregister_chrdev_region(gdev->dev_num, 1);
	return ret;
}

/**
 * @brief Driver remove function (cleanup).
 */
static int named_gpio_remove(struct platform_device *pdev)
{
	struct gpio_dev *gdev = platform_get_drvdata(pdev);

	dev_dbg(&pdev->dev, "Remove function called. Cleaning up\n");

	// Destroy device
	device_destroy(imsar_named_gpio_class, gdev->dev_num);

	// Delete the character device structure
	cdev_del(&gdev->cdev);

	// Unregister the major/minor number
	unregister_chrdev_region(gdev->dev_num, 1);

	// Note: All devm_ managed resources (GPIO descriptor, device file, driver data)
	// are automatically cleaned up here via the platform device's release function.

	return 0;
}

/*******************************************************************************
 * Platform Driver Structure
 ******************************************************************************/

// Match table for device tree node compatibility
static const struct of_device_id gpio_of_match[] = {
    {.compatible = "imsar,named-gpio"},
    {},
};
MODULE_DEVICE_TABLE(of, gpio_of_match);

// The platform driver definition
static struct platform_driver imsar_named_gpio_driver = {
    .probe = named_gpio_probe,
    .remove = named_gpio_remove,
    .driver =
        {
            .name = DRIVER_NAME,
            .of_match_table = of_match_ptr(gpio_of_match),
        },
};

/*******************************************************************************
 * Module Init and Exit Routines
 ******************************************************************************/

/**
 * @brief Module initialization: create the shared class and register the platform driver.
 */
static int __init imsar_named_gpio_init(void)
{
	int ret;

	// 1. Create the globally shared device class
	imsar_named_gpio_class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(imsar_named_gpio_class))
	{
		pr_err("%s: Failed to create device class\n", DRIVER_NAME);
		return PTR_ERR(imsar_named_gpio_class);
	}

	imsar_named_gpio_class->dev_groups = named_gpio_attr_groups;

	// 2. Register the platform driver
	ret = platform_driver_register(&imsar_named_gpio_driver);
	if (ret)
	{
		pr_err("%s: Failed to register platform driver\n", DRIVER_NAME);
		class_destroy(imsar_named_gpio_class); // Clean up the class on failure
	}

	return ret;
}

/**
 * @brief Module exit: unregister the platform driver and destroy the shared class.
 */
static void __exit imsar_named_gpio_exit(void)
{
	// 1. Unregister the platform driver
	platform_driver_unregister(&imsar_named_gpio_driver);

	// 2. Destroy the globally shared device class
	class_destroy(imsar_named_gpio_class);
}

/*******************************************************************************
 * Module Entry and Exit Points
 ******************************************************************************/

module_init(imsar_named_gpio_init);
module_exit(imsar_named_gpio_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("IMSAR, LLC.");
MODULE_DESCRIPTION("Exposes GPIO to user space as a character device");
MODULE_VERSION(GIT_DESCRIBE);