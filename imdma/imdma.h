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

struct imdma_buffer_spec
{
	unsigned int count;      // number of buffers
	unsigned int size_bytes; // size of each buffer
};

struct imdma_buffer_reserve_spec
{
	unsigned int buffer_index; // The index of the buffer allocated/reserved
	unsigned int offset_bytes; // The start offset (in bytes) of the buffer relative to the mmap'ed start address
};

struct imdma_transfer_start_spec
{
	unsigned int buffer_index; // REQUIRED: buffer_index return by driver from IMDMA_TRANSFER_RESERVE call
	unsigned int length_bytes; // REQUIRED: The length of the data to transfer in bytes
};

struct imdma_transfer_finish_spec
{
	unsigned int buffer_index; // REQUIRED: buffer_index return by driver from IMDMA_TRANSFER_RESERVE call
	unsigned int timeout_ms;   // REQUIRED: timeout in milliseconds; 0 will use the driver/DT default
};

struct imdma_buffer_release_spec
{
	unsigned int buffer_index; // REQUIRED: buffer_index return by driver from IMDMA_TRANSFER_RESERVE call
};


////////////////////////////////////
////////// IOCTL options ///////////
////////////////////////////////////

///////////////////////////////
// Buffer control/status
///////////////////////////////

// Retrieve the buffer specifications
//
// Return code:
//    0 on success
// Argument:
//    count will be populated with the number of buffers available
//    size_bytes will be populated with the size of each buffer
#define IMDMA_BUFFER_GET_SPEC _IOR('a', 'b', struct imdma_buffer_spec *) // get the buffer count, size

// #define IMDMA_BUFFER_SET_SPEC _IOW('a', 'd', struct imdma_buffer_spec *) // set the buffer count, size

///////////////////////////////
// Transfer actions
///////////////////////////////

// Reserve a buffer for a transfer
// If the DMA direction is host-to-device, the data can be populated after this call
//
// NOTE: The buffer must be released after use using IMDMA_TRANSFER_RELEASE
//
// Return code:
//    0 on success
//    -EINVAL if arg is invalid
//    -ENOBUFS if all buffers are reserved
// Argument:
//    buffer_index set by kernel on success (to be used by other ioctl calls)
//    offset_bytes set by the kernel driver to indicate the offset of the buffer relative to the mmap'ed memory
#define IMDMA_BUFFER_RESERVE _IOR('a', 'a', struct imdma_buffer_reserve_spec *)

// Start the DMA transfer (non-blocking)
//
// Return code:
//    0 on success
//    -ENOENT if buffer_index is invalid
//    -EPERM if the provided buffer_index is not reserved
//    -EOVERFLOW if length_bytes is larger than buffer_size
//    -EALREADY if a transfer is already in progress or queued for this buffer
// Argument:
//    buffer_index REQUIRED for kernel to know what buffer to use
//    length_bytes REQUIRED for kernel to know how much data to transfer
#define IMDMA_TRANSFER_START _IOW('a', 's', struct imdma_transfer_start_spec *)

// Wait for the DMA transfer to finish (blocking)
//
// This call will block waiting for the DMA transfer to complete
//
// Return code:
//    0 on success and completion
//    -ENOENT if buffer_index is invalid
//    -EPERM if not transfer has been started for the given buffer_index
//    -ETIMEDOUT if the timeout was reached before the transfer completed
//    -EIO if an error occurred during the transfer
// Argument:
//    buffer_index REQUIRED for the kernel driver to know what buffer to use
//    timeout_ms REQUIRED the maximum milliseconds to wait before giving up
#define IMDMA_TRANSFER_FINISH _IOW('a', 'w', struct imdma_transfer_finish_spec *)

// Release the buffer acquired from IMDMA_TRANSFER_RESERVE
//
// NOTE: This call will block if a transfer is currently in progress for the given buffer
//
// Return code:
//    0 on successful release
//    -EINVAL if arg is invalid
//    -ENOENT if buffer_index is invalid
//    -EPERM if the buffer was never reserved
//    -EBUSY if the buffer is still in use
// Argument:
//    buffer_index REQUIRED for the kernel driver to know what buffer to release/free
#define IMDMA_BUFFER_RELEASE _IOW('a', 'f', struct imdma_buffer_release_spec *)
