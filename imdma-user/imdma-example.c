// User space example demonstrating how to use the
// IMSAR DMA driver (imdma)

#include "libimdma.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


static void printHexDump(const void *start, unsigned int length_bytes)
{
	const unsigned char *buf = (unsigned char *)start;
	unsigned int byteCount = 0;
	for (unsigned int i = 0; i < length_bytes; i++)
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

static void print64BitLongs(const void *start, unsigned int length_bytes)
{
	const uint64_t *buf = (const uint64_t *)start;
	unsigned int word_count = length_bytes / sizeof(uint64_t);
	for (unsigned int i = 0; i < word_count; i++)
	{
		printf("%lu\n", buf[i]);
	}
}

int main(int argc, const char *const argv[])
{
	if (argc < 2)
	{
		printf("Usage: %s <device> [lengthBytes:1000] [print:off|u64|x|fl]\n", argv[0]);
		printf("Example: %s /dev/imdma_downsampled\n", argv[0]);
		return 1;
	}

	const char *devicePath = argv[1];

	imdma_t *imdma = imdma_create(devicePath);
	if (imdma == NULL)
	{
		return -1;
	}

	unsigned int lengthBytes = 1000;
	if (argc >= 3)
	{
		lengthBytes = strtoul(argv[2], NULL, 10);
	}

	const char *printType = argc >= 4 ? argv[3] : "off";

	// printf("Device: %s\n", devicePath);
	// printf("Length: %u\n", lengthBytes);
	// printf("Print: %s\n", printType);

	imdma_transfer_t *dmaTransfer = imdma_transfer_alloc(imdma);
	if (dmaTransfer == NULL)
	{
		fprintf(stderr, "failed to allocate a transfer buffer\n");
		return -1;
	}

	int setLenRc = imdma_transfer_set_length(dmaTransfer, lengthBytes);
	if (setLenRc != 0)
	{
		fprintf(stderr, "failed to set transfer buffer length\n");
		return -1;
	}

	// Request a single transfer
	int startRc = imdma_transfer_start_async(dmaTransfer);
	if (startRc != 0)
	{
		fprintf(stderr, "failed to start transfer\n");
		return -1;
	}

	// Wait for the transfer to finish
	int finishRc = imdma_transfer_finish(dmaTransfer);
	if (finishRc != 0)
	{
		fprintf(stderr, "failed to finish transfer\n");
		return -1;
	}

	const void *dmaBuffer = imdma_transfer_get_data_const(dmaTransfer);
	unsigned int dmaBufferLen = imdma_transfer_get_length(dmaTransfer);

	if (dmaBuffer == NULL || dmaBufferLen == 0)
	{
		fprintf(stderr, "bad transfer result: buf=%p, len=%u\n", dmaBuffer, dmaBufferLen);
		return -1;
	}

	// Use the data (access to this data MAY be uncached -- avoid repeated reads)
	if (strcmp(printType, "x") == 0)
	{
		printHexDump(dmaBuffer, dmaBufferLen);
	}
	else if (strcmp(printType, "u64") == 0)
	{
		print64BitLongs(dmaBuffer, dmaBufferLen);
	}
	else if (strcmp(printType, "fl") == 0)
	{
		uint64_t first, last;
		memcpy(&first, dmaBuffer, sizeof(first));
		memcpy(&last, dmaBuffer + dmaBufferLen - sizeof(last), sizeof(last));
		printf("%012" PRIu64 " to %012" PRIu64 "\n", first, last);
	}

	imdma_transfer_free(dmaTransfer);

	imdma_free(imdma);

	return 0;
}
