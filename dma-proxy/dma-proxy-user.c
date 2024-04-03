#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "dma-proxy.h"

const char *rx_channel_path = "/dev/dma_proxy_rx";
const char *output_file_name = "rx.bin";
struct channel_buffer *buf_ptr;
int fd;

#define RX_BUFFER_COUNT 4

static bool write_rx_file = false;
static int transfer_size = 0;
static volatile int stop = 0;
int num_transfers = 0;

void sigint(int a)
{
	stop = 1;
	signal(SIGINT, SIG_DFL); // restore default handler
}

static int inc_wrap(int current, int add, int wrap)
{
	return (current + add) % wrap;
}

void write_buffer_to_file(const char *file_name, const void *buffer, unsigned int length)
{
	int bin_fd = open(file_name, O_CREAT | O_TRUNC | O_RDWR);
	if (bin_fd == -1)
	{
		printf("write_buffer_to_file: failed to open %s\n", file_name);
		return;
	}
	ssize_t write_len = write(bin_fd, (void *)buffer, length);
	if (write_len != length)
	{
		printf("write_buffer_to_file: failed to write %u bytes (ret was %li)\n", length, write_len);
	}
	close(bin_fd);
}

void transfer_rx()
{
	int in_progress_count = 0;

	// Start all buffers being received
	for (int buffer_id = 0; buffer_id < RX_BUFFER_COUNT; buffer_id += 1)
	{
		// Set transfer size
		buf_ptr[buffer_id].length = transfer_size;

		// Clear the buffer
		unsigned char *buffer = buf_ptr[buffer_id].buffer;
		memset(buffer, 0xff, BUFFER_SIZE);

		// Tell DMA to start transfer
		ioctl(fd, START_XFER, &buffer_id);

		// Handle the case of a specified number of transfers that is less than the number
		// of buffers
		in_progress_count++;
		if (in_progress_count >= num_transfers)
		{
			break;
		}
	}

	int buffer_id = 0;
	int rx_counter = 0;

	// Finish each queued up receive buffer and keep starting the buffer over again
	// until all the transfers are done
	while (1)
	{
		ioctl(fd, FINISH_XFER, &buffer_id);

		if (buf_ptr[buffer_id].status != PROXY_NO_ERROR)
		{
			printf("rx: transfer error! # transfers %d, # completed %d, # in progress %d\n", num_transfers, rx_counter,
			       in_progress_count);
			break;
		}

		// Verify the data received matches what was sent (tx is looped back to tx)
		// A unique value in the buffers is used across all transfers
		if (write_rx_file)
		{
			unsigned char *buffer = buf_ptr[buffer_id].buffer;
			write_buffer_to_file(output_file_name, buffer, transfer_size);
		}

		// Keep track how many transfers are in progress so that only the specified number
		// of transfers are attempted
		in_progress_count--;

		// If all the transfers are done then exit
		if (++rx_counter >= num_transfers)
		{
			break;
		}

		// If the ones in progress will complete the number of transfers then don't start more
		// but finish the ones that are already started
		if ((rx_counter + in_progress_count) >= num_transfers)
		{
			buffer_id = inc_wrap(buffer_id, 1, RX_BUFFER_COUNT);
			continue;
		}

		// Start the next buffer again with another transfer keeping track of
		// the number in progress but not finished
		ioctl(fd, START_XFER, &buffer_id);

		in_progress_count++;

		// Flip to next buffer treating them as a circular list, and possibly skipping some
		// to show the results when prefetching is not happening
		buffer_id = inc_wrap(buffer_id, 1, RX_BUFFER_COUNT);
	}
}

// The main program starts the transmit thread and then does the receive processing to do a number of DMA transfers.
int main(int argc, char *argv[])
{
	signal(SIGINT, sigint);

	if (argc != 3 && argc != 4)
	{
		printf("Usage: %s <# of DMA transfers to perform> <# of bytes in each transfer in KB (< 1MB)> "
		       "<write rx file, 0 or 1>\n",
		       argv[0]);
		exit(EXIT_FAILURE);
	}

	// Get the number of transfers to perform
	num_transfers = atoi(argv[1]);

	// Get the size of the test to run, making sure it's not bigger than the size of the buffers and
	// convert it from KB to bytes
	transfer_size = atoi(argv[2]);
	if (transfer_size > BUFFER_SIZE || transfer_size < 0)
	{
		transfer_size = BUFFER_SIZE;
	}
	transfer_size *= 1024;

	if (argc == 4)
	{
		write_rx_file = (atoi(argv[3]) != 0);
	}

	fd = open(rx_channel_path, O_RDWR);
	if (fd < 1)
	{
		printf("Unable to open DMA proxy device file: %s\n", rx_channel_path);
		exit(EXIT_FAILURE);
	}

	buf_ptr = (struct channel_buffer *)mmap(NULL,                                            // req addr
	                                        sizeof(struct channel_buffer) * RX_BUFFER_COUNT, // length
	                                        PROT_READ | PROT_WRITE,                          // protection
	                                        MAP_SHARED,                                      // flags
	                                        fd,                                              // fd
	                                        0                                                // offset
	);
	if (buf_ptr == MAP_FAILED)
	{
		printf("failed\n");
		exit(EXIT_FAILURE);
	}

	transfer_rx();

	// Unmap rx channel
	munmap(buf_ptr, sizeof(struct channel_buffer));

	// Close rx channels
	close(fd);

	return 0;
}
