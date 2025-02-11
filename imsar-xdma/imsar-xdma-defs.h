/**
 * IMSAR driver for the Xilinx DMA Core in Simple Register Mode
 *
 *
 * Copyright (C) 2025 IMSAR, LLC.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 *
 */

#ifndef __IMSAR_XDMA_DEFS_H
#define __IMSAR_XDMA_DEFS_H

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/wait.h>

#define IMSAR_XDMA_DRIVER_NAME "imsar_xdma"
#define IMSAR_XDMA_MAX_CHANNELS 2

typedef enum imsar_xdma_dir_en
{
	IMSAR_XDMA_DIR_UNKNOWN,
	IMSAR_XDMA_DIR_S2MM,
	IMSAR_XDMA_DIR_MM2S,
} imsar_xdma_dir_t;

typedef struct imsar_xdma_dev_st imsar_xdma_dev_t;
typedef struct imsar_xdma_buffer_meta_st imsar_xdma_buffer_meta_t;
typedef struct imsar_xdma_chan_st imsar_xdma_channel_t;
typedef struct imsar_xdma_file_st imsar_xdma_file_t;

struct imsar_xdma_buffer_meta_st
{
	unsigned int transfer_id;
	unsigned int length;
	unsigned int offset;
};

struct imsar_xdma_dev_st
{
	struct platform_device *platform_device;
	struct device *device; // shortcut to platform_device.dev

	//  Device tree properties
	const char *name; // imsar,name

	// Register access
	void __iomem *regs; // ioremap'ed registers
	unsigned int log_register_access;

	// Character device
	dev_t char_dev_node; // Character device region
	struct cdev char_dev;

	// State
	imsar_xdma_channel_t *channels[IMSAR_XDMA_MAX_CHANNELS];
};

struct imsar_xdma_chan_st
{
	// Pointers to other data
	imsar_xdma_dev_t *xdma_device;
	struct device_node *device_node; // device tree node for channel

	// Device Tree properties
	unsigned int reg_offset;        // reg[0]
	unsigned int irq;               // Mapped IRQ
	const char *name;               // imsar,name
	imsar_xdma_dir_t direction;     // imsar,direction
	unsigned int buffer_count;      // imsar,buffer-count
	unsigned int buffer_size_bytes; // imsar,buffer-size-bytes

	// DMA buffer addresses
	void *buffer_virt_addr;
	dma_addr_t buffer_bus_addr;

	// Character device
	struct device *char_dev_device;

	// State
	unsigned int channel_index;
	unsigned int last_finished_transfer_id;
	unsigned int in_progress_transfer_id;
	imsar_xdma_buffer_meta_t *buffer_metadata; // kzalloc'd array for each buffer
	unsigned int log_transfer_events;

	// Consumers
	spinlock_t consumers_spinlock;    // held when changing consuming_files
	struct list_head consuming_files; // points at imsar_user_interrupt_file_t entries
};

struct imsar_xdma_file_st
{
	imsar_xdma_channel_t *channel;
	unsigned int last_read_transfer_id;
	wait_queue_head_t file_waitqueue;
	struct list_head list; // used to link pointers for consuming_files
};

#endif