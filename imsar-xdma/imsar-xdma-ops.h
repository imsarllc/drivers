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

#ifndef __IMSAR_XDMA_OPS_H
#define __IMSAR_XDMA_OPS_H

#include "imsar-xdma-defs.h"

// Registers
#define REG_CONTROL 0x00
#define REG_STATUS 0x04
#define REG_ADDR_LSB 0x18
#define REG_ADDR_MSB 0x1C
#define REG_LENGTH 0x28

// Control flags
#define FLAG_CONTROL_RUNSTOP BIT(0)
#define FLAG_CONTROL_RESET BIT(2)
#define FLAG_CONTROL_IOC_IRQ_EN BIT(12)
#define FLAG_CONTROL_ERR_IRQ_EN BIT(14)
#define FLAG_CONTROL_ALL_IRQ_EN (FLAG_CONTROL_IOC_IRQ_EN | FLAG_CONTROL_ERR_IRQ_EN)

// Status flags
#define FLAG_STATUS_HALTED BIT(0)
#define FLAG_STATUS_IDLE BIT(1)

#define FLAG_STATUS_DMA_INT_ERR BIT(4)
#define FLAG_STATUS_DMA_SLV_ERR BIT(5)
#define FLAG_STATUS_DMA_DEC_ERR BIT(6)
#define FLAG_STATUS_DMA_ALL_ERRS (FLAG_STATUS_DMA_INT_ERR | FLAG_STATUS_DMA_SLV_ERR | FLAG_STATUS_DMA_DEC_ERR)

#define FLAG_STATUS_IOC_IRQ BIT(12)
#define FLAG_STATUS_ERR_IRQ BIT(14)
#define FLAG_STATUS_ALL_IRQ (FLAG_STATUS_IOC_IRQ | FLAG_STATUS_ERR_IRQ)

u32 imsar_xdma_reg_read(imsar_xdma_dev_t *xdma_dev, unsigned int reg);
void imsar_xdma_reg_write(imsar_xdma_dev_t *xdma_dev, unsigned int reg, u32 value);

void imsar_xdma_reset(imsar_xdma_dev_t *xdma_dev);

u32 imsar_xdma_chan_reg_read(imsar_xdma_channel_t *channel, unsigned int reg);
void imsar_xdma_chan_reg_write(imsar_xdma_channel_t *channel, unsigned int reg, u32 value);
void imsar_xdma_chan_reg_bit_clr_set(imsar_xdma_channel_t *channel, unsigned int reg, u32 mask, u32 value);
void imsar_xdma_chan_irq_enable(imsar_xdma_channel_t *channel);
void imsar_xdma_chan_irq_disable(imsar_xdma_channel_t *channel);
void imsar_xdma_chan_irq_ack(imsar_xdma_channel_t *channel);
void imsar_xdma_chan_start(imsar_xdma_channel_t *channel);
void imsar_xdma_chan_stop(imsar_xdma_channel_t *channel);
void imsar_xdma_chan_set_addr_and_len(imsar_xdma_channel_t *channel, u32 address, u32 length);
u32 imsar_xdma_chan_read_len(imsar_xdma_channel_t *channel);

#endif