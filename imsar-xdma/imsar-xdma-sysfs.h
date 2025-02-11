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

#ifndef __IMSAR_XDMA_SYSFS_H
#define __IMSAR_XDMA_SYSFS_H

#include <linux/device.h>

extern const struct attribute_group *imsar_xdma_sysfs_attr_groups[];

ssize_t imsar_xdma_sysfs_name_show(struct device *dev, struct device_attribute *attr, char *buf);

ssize_t imsar_xdma_sysfs_buffer_count_show(struct device *dev, struct device_attribute *attr, char *buf);
ssize_t imsar_xdma_sysfs_buffer_size_show(struct device *dev, struct device_attribute *attr, char *buf);
ssize_t imsar_xdma_sysfs_transfer_id_show(struct device *dev, struct device_attribute *attr, char *buf);

ssize_t imsar_xdma_sysfs_log_register_access_show(struct device *dev, struct device_attribute *attr, char *buf);
ssize_t imsar_xdma_sysfs_log_register_access_store(struct device *dev, struct device_attribute *attr, const char *buf,
                                                  size_t size);
ssize_t imsar_xdma_sysfs_log_transfer_events_show(struct device *dev, struct device_attribute *attr, char *buf);
ssize_t imsar_xdma_sysfs_log_transfer_events_store(struct device *dev, struct device_attribute *attr, const char *buf,
                                                  size_t size);

ssize_t imsar_xdma_sysfs_status_idle_show(struct device *dev, struct device_attribute *attr, char *buf);
ssize_t imsar_xdma_sysfs_status_halted_show(struct device *dev, struct device_attribute *attr, char *buf);
ssize_t imsar_xdma_sysfs_status_err_int_show(struct device *dev, struct device_attribute *attr, char *buf);
ssize_t imsar_xdma_sysfs_status_err_slv_show(struct device *dev, struct device_attribute *attr, char *buf);
ssize_t imsar_xdma_sysfs_status_err_dec_show(struct device *dev, struct device_attribute *attr, char *buf);
ssize_t imsar_xdma_sysfs_status_irq_ioc_show(struct device *dev, struct device_attribute *attr, char *buf);
ssize_t imsar_xdma_sysfs_status_irq_err_show(struct device *dev, struct device_attribute *attr, char *buf);

ssize_t imsar_xdma_sysfs_control_reset_show(struct device *dev, struct device_attribute *attr, char *buf);
ssize_t imsar_xdma_sysfs_control_reset_store(struct device *dev, struct device_attribute *attr, const char *buf,
                                            size_t size);
ssize_t imsar_xdma_sysfs_control_runstop_show(struct device *dev, struct device_attribute *attr, char *buf);
ssize_t imsar_xdma_sysfs_control_runstop_store(struct device *dev, struct device_attribute *attr, const char *buf,
                                              size_t size);
ssize_t imsar_xdma_sysfs_control_irq_ioc_en_show(struct device *dev, struct device_attribute *attr, char *buf);
ssize_t imsar_xdma_sysfs_control_irq_ioc_en_store(struct device *dev, struct device_attribute *attr, const char *buf,
                                                 size_t size);
ssize_t imsar_xdma_sysfs_control_irq_err_en_show(struct device *dev, struct device_attribute *attr, char *buf);
ssize_t imsar_xdma_sysfs_control_irq_err_en_store(struct device *dev, struct device_attribute *attr, const char *buf,
                                                 size_t size);

ssize_t imsar_xdma_sysfs_reg_control_show(struct device *dev, struct device_attribute *attr, char *buf);
ssize_t imsar_xdma_sysfs_reg_control_store(struct device *dev, struct device_attribute *attr, const char *buf,
                                          size_t size);
ssize_t imsar_xdma_sysfs_reg_status_show(struct device *dev, struct device_attribute *attr, char *buf);
ssize_t imsar_xdma_sysfs_reg_status_store(struct device *dev, struct device_attribute *attr, const char *buf,
                                         size_t size);
ssize_t imsar_xdma_sysfs_reg_address_show(struct device *dev, struct device_attribute *attr, char *buf);
ssize_t imsar_xdma_sysfs_reg_address_store(struct device *dev, struct device_attribute *attr, const char *buf,
                                          size_t size);
ssize_t imsar_xdma_sysfs_reg_length_show(struct device *dev, struct device_attribute *attr, char *buf);
ssize_t imsar_xdma_sysfs_reg_length_store(struct device *dev, struct device_attribute *attr, const char *buf,
                                         size_t size);

#endif