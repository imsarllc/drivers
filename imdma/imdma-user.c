// User space example demonstrating how to use the
// IMSAR DMA driver (imdma)

#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "imdma.h"

static void printHexDump(const void *start, unsigned int length)
{
	const unsigned char *buf = (unsigned char *)start;
	unsigned int byteCount = 0;
	for (unsigned int i = 0; i < length; i++)
	{
		if ((byteCount % 32) == 0)
		{
			printf("\n");
		}
		printf("%02x ", buf[i]);
		byteCount++;
	}
}

int main(int argc, const char *const argv[])
{
	if (argc < 2)
	{
		printf("Usage: %s <device>\n", argv[0]);
		printf("Example: %s /dev/imdma/downsampled\n", argv[0]);
		return 1;
	}

	const char *devicePath = argv[1];

	// Open the device
	int devfd = open(devicePath, "r");
	if (devfd != 0)
	{
		perror("failed to open device");
		return 1;
	}

	// Read the buffer specifications (count and size)
	struct imdma_buffer_spec bufferSpec;
	int getSpecResult = ioctl(devfd, IMDMA_BUFFER_GET_SPEC, &bufferSpec);
	if (getSpecResult < 0)
	{
		perror("failed to get buffer specifications");
		return 1;
	}

	// Map the memory into user space
	unsigned char *buffer = mmap(NULL,                               // requested address
	                             bufferSpec.count * bufferSpec.size, // mapped size
	                             PROT_READ | PROT_WRITE,             // protections
	                             MAP_SHARED,                         // flags
	                             devfd,                              // file descriptor
	                             0);                                 // offset
	if (buffer == MAP_FAILED)
	{
		perror("failed to get buffer specifications");
		return 1;
	}

	// Configure the transfer
	const unsigned int TRANSFER_SIZE = 1000; // number of 8-byte transactions
	struct imdma_transfer_spec transferSpec = {
	    .buffer_index = 0,      //
	    .length = TRANSFER_SIZE //
	};

	// Start the transfer
	// Note: This ioctl requires transferSpec.buffer_index, transferSpec.length
	int startResult = ioctl(devfd, IMDMA_TRANSFER_START, &transferSpec);
	if (startResult < 0)
	{
		perror("failed to start transfer");
		return 1;
	}

	// Wait for transfer to finish
	// Note: This ioctl requires transferSpec.buffer_index
	//       It sets transferSpec.{status, offset, length}
	int finishResult = ioctl(devfd, IMDMA_TRANSFER_FINISH, &transferSpec);
	if (finishResult < 0)
	{
		perror("failed to finish transfer");
		return 1;
	}

	if (transferSpec.status != IMDMA_STATUS_COMPLETE)
	{
		printf("Transfer failed: status = %u\n", transferSpec.status);
		return 1;
	}

	// Use the data (access to this data MAY be uncached -- avoid repeated reads)
	printHexDump(&buffer[transferSpec.offset], transferSpec.length);

	// Unmap rx channel
	munmap(buffer, bufferSpec.count * bufferSpec.size);

	// Close the device
	close(devfd);

	return 0;
}
