/*
 * TTY on a LCD connected to I2C
 * Supports Newhaven NHD-0216K3Z-NSW-BBW
 *
 *  Copyright (C) 2013 Altera Corporation.  All rights reserved.
 *  https://github.com/altera-opensource/linux-socfpga/blob/socfpga-4.19/drivers/tty/newhaven_lcd.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define DRV_NAME "lcd-comm"
#define DEV_NAME "ttyLCD"

#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/delay.h>

#define LCD_COMMAND             0x0

#ifdef DEBUG
#define LCD_DISPLAY_ON          0x0f
#else
#define LCD_DISPLAY_ON          0x0c
#endif

#define LCD_FUNCTION_SET_MASK   0x20
#define LCD_FUNCTION_8BIT_MASK  0x10
#define LCD_FUNCTION_2LINE_MASK 0x08
#define LCD_FUNCTION_FONT_MASK  0x04
#define LCD_FUNCTION_IS_MASK    0x01

//1/4 bias, no adjustment
#define LCD_OSC_FREQ            0x14
// Constrast = 0xF (highest)
#define LCD_CONTRAST            0x7F
//Power/Icon/constrast
#define LCD_PWR_ICON_CONTRAST   0x5F
//Follower Control
#define LCD_FOLLOWER            0x6A
#define LCD_MOVE_DIRECTION      0x06



#define LCD_DISPLAY_OFF         0x08
#define LCD_SET_CURSOR_MASK     0x80
#define LCD_CLEAR_SCREEN        0x01

#define ASCII_BS                0x08
#define ASCII_LF                0x0a
#define ASCII_CR                0x0d
#define ASCII_ESC               0x1b
#define ASCII_SPACE             0x20
#define ASCII_BACKSLASH         0x5c
#define ASCII_TILDE             0x7e
#define ASCII_DEL               0x7f

/* Array of commands to send to set up custom fonts. */
struct lcd {
	struct device *dev;
	struct i2c_client *client;
	struct tty_driver *lcd_tty_driver;
	struct tty_port port;
	char *buffer;
	unsigned int top_line;
	unsigned int height;
	unsigned int width;
	unsigned int cursor_line;
	unsigned int cursor_col;
};

#define MAX_LCDS 1
static struct lcd lcd_data_static[MAX_LCDS];

static void print_cmd(u8 *buf, u8 len)
{
#ifdef DEBUG
	int ii;
	printk(KERN_DEBUG "i2c cmd: ");
	for (ii = 0; ii < len; ii++)
	{
		printk(KERN_CONT"%02x ", buf[ii]);
	}
	printk(KERN_CONT "\n");
#endif
}

static int lcd_cmd(struct lcd *lcd_data, u8 cmd)
{
	int count;
	u8 buf[2] = {LCD_COMMAND, cmd};

	count = i2c_master_send(lcd_data->client, buf, sizeof(buf));
	print_cmd(buf, 1);
	if (count != sizeof(buf)) {
		pr_err("%s: i2c_master_send returns %d\n", __func__, count);
		return -1;
	}
	msleep(1);
	return 0;
}

static int lcd_cmd_display_on(struct lcd *lcd_data)
{
	return lcd_cmd(lcd_data, LCD_DISPLAY_ON);
}

static int lcd_cmd_display_off(struct lcd *lcd_data)
{
	return lcd_cmd(lcd_data, LCD_DISPLAY_OFF);
}

static int lcd_cmd_clear_screen(struct lcd *lcd_data)
{
	return lcd_cmd(lcd_data, LCD_CLEAR_SCREEN);
}

static void lcd_cmd_setup(struct lcd *lcd_data)
{
	u8 buf[] = {
			LCD_COMMAND,
			LCD_FUNCTION_SET_MASK |
				LCD_FUNCTION_8BIT_MASK |
				LCD_FUNCTION_2LINE_MASK|
				LCD_FUNCTION_IS_MASK   ,
			LCD_OSC_FREQ,
			LCD_CONTRAST,
			LCD_PWR_ICON_CONTRAST,
			LCD_FOLLOWER,
			LCD_MOVE_DIRECTION
	};
	print_cmd(buf, sizeof(buf));
	i2c_master_send(lcd_data->client, buf, sizeof(buf));
}

/* From NHD-0216K3Z-NSW-BBY Display Module datasheet. */
#define LCD_CURSOR_LINE_MULTIPLIER 0x40

static int lcd_cmd_set_cursor(struct lcd *lcd_data, u8 line, u8 col)
{
	u8 cursor;

	BUG_ON((line >= lcd_data->height) || (col >= lcd_data->width));

	cursor = col + (LCD_CURSOR_LINE_MULTIPLIER * line);
	return lcd_cmd(lcd_data, LCD_SET_CURSOR_MASK | cursor);
}

/*
 * Map a line on the lcd display to a line on the buffer.
 * Note that the top line on the display (line 0) may not be line 0 on the
 * buffer due to scrolling.
 */
static unsigned int lcd_line_to_buf_line(struct lcd *lcd_data,
					 unsigned int line)
{
	unsigned int buf_line;

	buf_line = line + lcd_data->top_line;

	if (buf_line >= lcd_data->height)
		buf_line -= lcd_data->height;

	return buf_line;
}

/* Returns a pointer to the line, column position in the lcd buffer */
static char *lcd_buf_pointer(struct lcd *lcd_data, unsigned int line,
			     unsigned int col)
{
	unsigned int buf_line;
	char *buf;

	if ((lcd_data->cursor_line >= lcd_data->height) ||
	    (lcd_data->cursor_col >= lcd_data->width))
		return lcd_data->buffer;

	buf_line = lcd_line_to_buf_line(lcd_data, line);

	buf = lcd_data->buffer + (buf_line * lcd_data->width) + col;

	return buf;
}

static void lcd_clear_buffer_line(struct lcd *lcd_data, int line)
{
	char *buf;

	BUG_ON(line >= lcd_data->height);

	buf = lcd_buf_pointer(lcd_data, line, 0);
	memset(buf, ASCII_SPACE, lcd_data->width);
}

static void lcd_clear_buffer(struct lcd *lcd_data)
{
	memset(lcd_data->buffer, ASCII_SPACE,
	       lcd_data->width * lcd_data->height);
	lcd_data->cursor_line = 0;
	lcd_data->cursor_col = 0;
	lcd_data->top_line = 0;
}

static void lcd_reprint_one_line(struct lcd *lcd_data, u8 line)
{
	char cmd_buf[17];
	cmd_buf[0] = 0x40;

	memcpy(&cmd_buf[1], lcd_buf_pointer(lcd_data, line, 0), 16);

	lcd_cmd_set_cursor(lcd_data, line, 0);
	i2c_master_send(lcd_data->client, cmd_buf, lcd_data->width);
	print_cmd(cmd_buf, 17);

}

static void lcd_print_top_n_lines(struct lcd *lcd_data, u8 lines)
{
	unsigned int disp_line = 0;

	while (disp_line < lines)
		lcd_reprint_one_line(lcd_data, disp_line++);
}

static void lcd_add_char_at_cursor(struct lcd *lcd_data, char val)
{
	char *buf;

	buf = lcd_buf_pointer(lcd_data, lcd_data->cursor_line,
			lcd_data->cursor_col);

	*buf = val;

	if (lcd_data->cursor_col < (lcd_data->width - 1))
		lcd_data->cursor_col++;
}

static void lcd_rm_char_at_cursor(struct lcd *lcd_data)
{
	char *buf;

	if (lcd_data->cursor_col > 0)
		lcd_data->cursor_col--;

	buf = lcd_buf_pointer(lcd_data, lcd_data->cursor_line,
			lcd_data->cursor_col);
	*buf = ASCII_SPACE;
	lcd_reprint_one_line(lcd_data, lcd_data->cursor_line);

}

static void lcd_crlf(struct lcd *lcd_data)
{
	if (lcd_data->cursor_line < (lcd_data->height - 1)) {
		/* Next line is blank, carriage return to beginning of line. */
		lcd_data->cursor_line++;
		if (lcd_data->cursor_line >= lcd_data->height)
			lcd_data->cursor_line = 0;

	} else {
		/* Display is full.  Scroll up one line. */
		lcd_data->top_line++;
		if (lcd_data->top_line >= lcd_data->height)
			lcd_data->top_line = 0;

		lcd_cmd_clear_screen(lcd_data);
		lcd_clear_buffer_line(lcd_data, lcd_data->cursor_line);
		lcd_print_top_n_lines(lcd_data, lcd_data->height);
	}

	lcd_cmd_set_cursor(lcd_data, lcd_data->height - 1, 0);
	lcd_data->cursor_col = 0;
}

static void lcd_backspace(struct lcd *lcd_data)
{
	lcd_rm_char_at_cursor(lcd_data);
}

static int lcd_write(struct tty_struct *tty, const unsigned char *buf,
		     int count)
{
	struct lcd *lcd_data = tty->driver_data;
	int buf_i = 0, left;
	char val;

#ifdef DEBUG
	char *dbgbuf = kzalloc(count + 1, GFP_KERNEL);
	strncpy(dbgbuf, buf, count);
	pr_debug("\n%s: count=%d buf[0]=%02x --->%s<---\n", __func__, count,
		 buf[0], dbgbuf);
#endif /* DEBUG */

	if (count == 0) {
#ifdef DEBUG
		kfree(dbgbuf);
#endif /* DEBUG */
		return 0;
	}

	while (buf_i < count) {
		left = count - buf_i;

		/* process displayable chars */
		if ((0x20 <= buf[buf_i]) && (buf[buf_i] <= 0x7e)) {
			while ((buf_i < count) &&
			       ((0x20 <= buf[buf_i]) && (buf[buf_i] <= 0x7e))) {
				val = buf[buf_i];
				lcd_add_char_at_cursor(lcd_data, val);
				buf_i++;
			}

			/* flush the line out to the display when we get to eol */
			lcd_reprint_one_line(lcd_data, lcd_data->cursor_line);

		/*
		 * ECMA-48 CSI sequences (from console_codes man page)
		 *
		 * ESC [ 2 J : erase whole display.
		 * ESC [ 2 K : erase whole line.
		 */
		} else if (buf[buf_i] == ASCII_ESC) {
			if ((left >= 4) &&
				(buf[buf_i + 1] == '[') &&
				(buf[buf_i + 2] == '2') &&
				(buf[buf_i + 3] == 'J')) {
				pr_debug("ESC [2J = clear screan\n");
				lcd_clear_buffer(lcd_data);
				lcd_cmd_clear_screen(lcd_data);
				buf_i += 4;

			} else if ((left >= 4) &&
				(buf[buf_i + 1] == '[') &&
				(buf[buf_i + 2] == '2') &&
				(buf[buf_i + 3] == 'K')) {
				pr_debug("ESC [2K = clear line\n");
				lcd_clear_buffer_line(lcd_data, lcd_data->cursor_line);
				lcd_reprint_one_line(lcd_data, lcd_data->cursor_line);
				lcd_cmd_set_cursor(lcd_data, lcd_data->cursor_line, 0);
				lcd_data->cursor_col = 0;
				buf_i += 4;

			} else {
				pr_debug("Unsupported escape sequence\n");
				buf_i++;
			}

		} else if ((left >= 2) &&
			(buf[buf_i] == ASCII_CR) && (buf[buf_i + 1] == ASCII_LF)) {
			pr_debug("ASCII_CR/LF\n");
			lcd_crlf(lcd_data);
			buf_i += 2;

		} else if ((left >= 1) && (buf[buf_i] == ASCII_CR)) {
			pr_debug("ASCII_CR\n");
			lcd_crlf(lcd_data);
			buf_i++;

		} else if ((left >= 1) && (buf[buf_i] == ASCII_LF)) {
			pr_debug("ASCII_LF\n");
			lcd_crlf(lcd_data);
			buf_i++;

		} else if ((left >= 1) &&
				(buf[buf_i] == ASCII_DEL || buf[buf_i] == ASCII_BS)) {
			pr_debug("ASCII_BS\n");
			lcd_backspace(lcd_data);
			buf_i++;

		} else {
			pr_debug("%s - Unsupported command 0x%02x\n", __func__, buf[buf_i]);
			buf_i++;
		}
	}

#ifdef DEBUG
	kfree(dbgbuf);
#endif /* DEBUG */
	return count;
}


static int lcd_install(struct tty_driver *driver, struct tty_struct *tty)
{
	struct lcd *lcd_data;

	lcd_data = &lcd_data_static[tty->index];
	if (lcd_data == NULL)
		return -ENODEV;

	tty->driver_data = lcd_data;

	return tty_port_install(&lcd_data->port, driver, tty);
}

static int lcd_open(struct tty_struct *tty, struct file *filp)
{
	struct lcd *lcd_data = tty->driver_data;
	unsigned long flags;

	tty->driver_data = lcd_data;
	spin_lock_irqsave(&lcd_data->port.lock, flags);
	lcd_data->port.count++;
	spin_unlock_irqrestore(&lcd_data->port.lock, flags);
	tty_port_tty_set(&lcd_data->port, tty);

	return 0;
}

static void lcd_close(struct tty_struct *tty, struct file *filp)
{
	struct lcd *lcd_data = tty->driver_data;
	unsigned long flags;
	bool last;

	spin_lock_irqsave(&lcd_data->port.lock, flags);
	--lcd_data->port.count;
	last = (lcd_data->port.count == 0);
	spin_unlock_irqrestore(&lcd_data->port.lock, flags);
	if (last)
		tty_port_tty_set(&lcd_data->port, NULL);
}

static unsigned int lcd_write_room(struct tty_struct *tty)
{
	struct lcd *lcd_data = tty->driver_data;

	return lcd_data->height * lcd_data->width;
}

static const struct tty_operations lcd_ops = {
	.install         = lcd_install,
	.open            = lcd_open,
	.close           = lcd_close,
	.write           = lcd_write,
	.write_room      = lcd_write_room,
};

static int lcd_probe(struct i2c_client *client,
			const struct i2c_device_id *i2c_id)
{
	struct device_node *np = client->dev.of_node;
	struct lcd *lcd_data;
	struct tty_driver *lcd_tty_driver;
	unsigned int width = 16, height = 2, i;
	char *buffer;
	int ret = -ENOMEM;

	dev_info(&client->dev, "Probing LCD driver\n");
	of_property_read_u32(np, "height", &height);
	of_property_read_u32(np, "width", &width);
	if ((width == 0) || (height == 0)) {
		dev_err(&client->dev,
			"Need to specify lcd width/height in device tree\n");
		ret = -EINVAL;
		goto err_devtree;
	}

	for (i = 0 ; i < MAX_LCDS ; i++)
		if (lcd_data_static[i].client == NULL)
			break;
	if (i >= MAX_LCDS) {
		ret = -ENODEV;
		dev_warn(&client->dev,
			 "More than %d I2C LCD displays found. Giving up.\n",
			 MAX_LCDS);
		goto err_devtree;
	}
	lcd_data = &lcd_data_static[i];

	buffer = kzalloc(height * width, GFP_KERNEL);
	if (!buffer)
		goto err_devtree;

	i2c_set_clientdata(client, lcd_data);

	lcd_data->client  = client;
	lcd_data->dev     = &client->dev;
	lcd_data->height  = height;
	lcd_data->width   = width;
	lcd_data->buffer  = buffer;

	dev_set_drvdata(&client->dev, lcd_data);
	tty_port_init(&lcd_data->port);
	lcd_tty_driver = tty_alloc_driver(MAX_LCDS, 0);
	if (IS_ERR(lcd_tty_driver))
	{
		return PTR_ERR(lcd_tty_driver);
	}

	lcd_tty_driver->driver_name  = DRV_NAME;
	lcd_tty_driver->name         = DEV_NAME;
	lcd_tty_driver->type         = TTY_DRIVER_TYPE_SERIAL;
	lcd_tty_driver->subtype      = SERIAL_TYPE_NORMAL;
	lcd_tty_driver->init_termios = tty_std_termios;
	tty_set_operations(lcd_tty_driver, &lcd_ops);

	ret = tty_register_driver(lcd_tty_driver);
	if (ret)
		goto err_register;

	lcd_data->lcd_tty_driver = lcd_tty_driver;

	lcd_clear_buffer(lcd_data);
	lcd_cmd_setup(lcd_data);
	lcd_cmd_display_on(lcd_data);
	lcd_cmd_clear_screen(lcd_data);


	dev_info(&client->dev, "LCD driver initialized\n");

	return 0;

err_register:
	tty_driver_kref_put(lcd_data->lcd_tty_driver);
err_driver:
	kfree(buffer);
err_devtree:
	return ret;
}

static int __exit lcd_remove(struct i2c_client *client)
{
	struct lcd *lcd_data = i2c_get_clientdata(client);

	lcd_cmd_display_off(lcd_data);

	tty_unregister_driver(lcd_data->lcd_tty_driver);
	tty_driver_kref_put(lcd_data->lcd_tty_driver);
	kfree(lcd_data->buffer);

	return 0;
}

static const struct of_device_id lcd_of_match[] = {
		{.compatible = "newhaven,nhd-C0216ciz-nsw-fbw", },
		{},
};

static const struct i2c_device_id lcd_id[] = {
	{ DRV_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, lcd_id);

static struct i2c_driver lcd_i2c_driver = {
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = lcd_of_match,
	},
	.probe = lcd_probe,
	.remove = lcd_remove,
	.id_table = lcd_id,
};

static int __init lcd_init(void)
{
	return i2c_add_driver(&lcd_i2c_driver);
}
subsys_initcall(lcd_init);

static void __exit lcd_exit(void)
{
	i2c_del_driver(&lcd_i2c_driver);
}
module_exit(lcd_exit);

MODULE_DESCRIPTION("LCD 2x16");
MODULE_LICENSE("GPL");
