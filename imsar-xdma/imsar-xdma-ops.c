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

#include "imsar-xdma-ops.h"

u32 imsar_xdma_reg_read(imsar_xdma_dev_t *xdma_dev, unsigned int reg)
{
	u32 val = readl(xdma_dev->regs + reg);
	if (xdma_dev->log_register_access)
	{
		dev_dbg(xdma_dev->device, "reg_read(0x%08x) = 0x%08x", reg, val);
	}
	return val;
}

void imsar_xdma_reg_write(imsar_xdma_dev_t *xdma_dev, unsigned int reg, u32 value)
{
	if (xdma_dev->log_register_access)
	{
		dev_dbg(xdma_dev->device, "reg_write(0x%08x, 0x%08x)", reg, value);
	}
	writel(value, xdma_dev->regs + reg);
}

void imsar_xdma_reset(imsar_xdma_dev_t *xdma_dev)
{
	imsar_xdma_reg_write(xdma_dev, REG_CONTROL, FLAG_CONTROL_RESET);
}

u32 imsar_xdma_chan_reg_read(imsar_xdma_channel_t *channel, unsigned int reg)
{
	return imsar_xdma_reg_read(channel->xdma_device, channel->reg_offset + reg);
}

void imsar_xdma_chan_reg_write(imsar_xdma_channel_t *channel, unsigned int reg, u32 value)
{
	imsar_xdma_reg_write(channel->xdma_device, channel->reg_offset + reg, value);
}

void imsar_xdma_chan_reg_bit_clr_set(imsar_xdma_channel_t *channel, unsigned int reg, u32 mask, u32 value)
{
	u32 val;
	val = imsar_xdma_chan_reg_read(channel, reg);
	val &= ~mask;
	val |= value;
	imsar_xdma_chan_reg_write(channel, reg, val);
}

void imsar_xdma_chan_irq_enable(imsar_xdma_channel_t *channel)
{
	if (channel->log_transfer_events)
	{
		dev_dbg(channel->xdma_device->device, "%s: irq enable", channel->name);
	}
	imsar_xdma_chan_reg_bit_clr_set(channel, REG_CONTROL, FLAG_CONTROL_ALL_IRQ_EN, FLAG_CONTROL_ALL_IRQ_EN);
}

void imsar_xdma_chan_irq_disable(imsar_xdma_channel_t *channel)
{
	if (channel->log_transfer_events)
	{
		dev_dbg(channel->xdma_device->device, "%s irq disable", channel->name);
	}
	imsar_xdma_chan_reg_bit_clr_set(channel, REG_CONTROL, FLAG_CONTROL_ALL_IRQ_EN, 0);
}

void imsar_xdma_chan_irq_ack(imsar_xdma_channel_t *channel)
{
	if (channel->log_transfer_events)
	{
		dev_dbg(channel->xdma_device->device, "%s irq ack", channel->name);
	}
	imsar_xdma_chan_reg_bit_clr_set(channel, REG_STATUS, FLAG_STATUS_ALL_IRQ, FLAG_STATUS_ALL_IRQ);
}

void imsar_xdma_chan_start(imsar_xdma_channel_t *channel)
{
	if (channel->log_transfer_events)
	{
		dev_dbg(channel->xdma_device->device, "%s channel start", channel->name);
	}
	imsar_xdma_chan_reg_bit_clr_set(channel, REG_CONTROL, FLAG_CONTROL_RUNSTOP, FLAG_CONTROL_RUNSTOP);
}

void imsar_xdma_chan_stop(imsar_xdma_channel_t *channel)
{
	if (channel->log_transfer_events)
	{
		dev_dbg(channel->xdma_device->device, "%s channel stop", channel->name);
	}
	imsar_xdma_chan_reg_bit_clr_set(channel, REG_CONTROL, FLAG_CONTROL_RUNSTOP, 0);
}

void imsar_xdma_chan_set_addr_and_len(imsar_xdma_channel_t *channel, u32 address, u32 length)
{
	imsar_xdma_chan_reg_write(channel, REG_ADDR_LSB, address);
	imsar_xdma_chan_reg_write(channel, REG_LENGTH, length); // must be written last
}

u32 imsar_xdma_chan_read_len(imsar_xdma_channel_t *channel)
{
	return imsar_xdma_chan_reg_read(channel, REG_LENGTH);
}
