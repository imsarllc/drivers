/**
 * IMSAR User Space DMA driver
 *
 * Copyright (C) 2024 IMSAR, LLC.
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

enum imdma_transfer_status
{
	IMDMA_STATUS_PENDING = 0,
	// IMDMA_STATUS_IN_PROGRESS = 1,
	IMDMA_STATUS_TIMEOUT = 2,
	IMDMA_STATUS_ERROR = 3,
	IMDMA_STATUS_COMPLETE = 4
};

struct imdma_buffer_spec
{
	unsigned int count; // number of buffers
	unsigned int size;  // size of each buffer in bytes
};

struct imdma_transfer_spec
{
	// This field must be set by the user before all IMDMA_TRANSFER_* ioctl calls
	unsigned int buffer_index; // buffer to use (must be < number of buffers)

	// This field must be set by the user before IMDMA_TRANSFER_NOW, IMDMA_TRANSFER_START
	unsigned int length; // requested number of bytes to transfer

	// This field must be set by the user before IMDMA_TRANSFER_FINISH
	unsigned int timeout_ms; // requested timeout in milliseconds; must be > 0

	// These fields are set by ioctl calls: IMDMA_TRANSFER_NOW, IMDMA_TRANSFER_FINISH
	// They do not need to be set by the user
	enum imdma_transfer_status status; // status of the transfer
	unsigned int offset;               // start offset of transferred data
};

// IOCTL options

// Transfer ioctls
// #define IMDMA_TRANSFER_NOW _IOW('a', 'n', struct imdma_transfer_spec *)    // start and finish a transfer (blocking)
#define IMDMA_TRANSFER_START _IOW('a', 's', struct imdma_transfer_spec *)  // start a transfer (non-blocking)
// #define IMDMA_TRANSFER_CHECK _IOW('a', 'e', struct imdma_transfer_spec *)  // check status of transfer (non-blocking)
#define IMDMA_TRANSFER_FINISH _IOW('a', 'f', struct imdma_transfer_spec *) // wait for a transfer to finish (blocking)
// #define IMDMA_TRANSFER_CANCEL _IOW('a', 'c', struct imdma_transfer_spec *) // cancel an transfer (non-blocking)

// Buffer control/status ioctls
#define IMDMA_BUFFER_GET_SPEC _IOW('a', 'b', struct imdma_buffer_spec *) // get the buffer count, size
// #define IMDMA_BUFFER_SET_SPEC _IOW('a', 'd', struct imdma_buffer_spec *) // set the buffer count, size
