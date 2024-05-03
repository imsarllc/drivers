// User space example demonstrating how to use the
// IMSAR DMA driver (imdma)

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

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

	printf("\n");
}

static void printUnsignedLongs(const void *start, unsigned int length)
{
	const uint64_t *data = (const uint64_t *)start;
	unsigned int count = length / sizeof(uint64_t);
	for (unsigned int i = 0; i < count; i++)
	{
		printf("%lu\n", data[i]);
	}
}

int main(int argc, const char *const argv[])
{
	if (argc < 2)
	{
		printf("Usage: %s <device> [size:1000] [index=0]\n", argv[0]);
		printf("Example: %s /dev/imdma/downsampled\n", argv[0]);
		return 1;
	}

	const char *devicePath = argv[1];

	unsigned int size = 1000;
	if (argc >= 3)
	{
		size = strtoul(argv[2], NULL, 10);
	}

	unsigned int bufferIndex = 0;
	if (argc >= 4)
	{
		bufferIndex = strtoul(argv[3], NULL, 10);
	}

	// Open the device
	int devfd = open(devicePath, 0);
	if (devfd < 0)
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
	                             PROT_READ,                          // protections
	                             MAP_SHARED,                         // flags
	                             devfd,                              // file descriptor
	                             0);                                 // offset
	if (buffer == MAP_FAILED)
	{
		perror("mmap");
		return 1;
	}

	// Configure the transfer
	struct imdma_transfer_spec transferSpec = {
	    .buffer_index = bufferIndex, //
	    .length = size               //
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
	// printHexDump(&buffer[transferSpec.offset], transferSpec.length);

	printUnsignedLongs(&buffer[transferSpec.offset], transferSpec.length);

	// Unmap rx channel
	munmap(buffer, bufferSpec.count * bufferSpec.size);

	// Close the device
	close(devfd);

	return 0;
}
