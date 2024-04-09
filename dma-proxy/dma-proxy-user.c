#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "dma-proxy.h"

const char *rx_channel_path = "/dev/dma_proxy_rx";
struct channel_buffer *channel_buffers;
int fd;

#define RX_BUFFER_COUNT 4 // must be <= driver BUFFER_COUNT

static bool write_rx_files = true;
static int transfer_size = 0;
static int num_transfers = 0;
static int buffers_status[BUFFER_COUNT]; // 0 if free, > 0 if busy (with lower numbers being started earlier)

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

void start_transfer(int buffer_id)
{
	static int transfer_id = 0;

	// Set transfer size
	channel_buffers[buffer_id].length = transfer_size;

	// Clear the buffer
	memset(channel_buffers[buffer_id].buffer, 0xff, BUFFER_SIZE);

	// Tell DMA to start transfer
	printf("enqueue transfer of buffer %u... ", buffer_id);
	ioctl(fd, START_XFER, &buffer_id);
	printf("enqueued\n");

	transfer_id++;
	buffers_status[buffer_id] = transfer_id; // set status busy
}

void wait_for_transfer(int buffer_id)
{
	printf("wait for transfer of buffer %u to finish... ", buffer_id);
	ioctl(fd, FINISH_XFER, &buffer_id);
	printf("finished\n");
}

int start_transfer_maybe()
{
	for (int buffer_id = 0; buffer_id < BUFFER_COUNT; buffer_id++)
	{
		if (buffers_status[buffer_id] == 0) // free
		{
			start_transfer(buffer_id);
			return buffer_id;
		}
	}
	return -1;
}

int wait_for_first_transfer()
{
	int lowest_index = -1;
	int lowest_cnt = INT_MAX;
	for (int buffer_id = 0; buffer_id < BUFFER_COUNT; buffer_id++)
	{
		if (buffers_status[buffer_id] != 0 && buffers_status[buffer_id] < lowest_cnt)
		{
			lowest_index = buffer_id;
			lowest_cnt = buffers_status[buffer_id];
		}
	}
	if (lowest_index == -1)
	{
		return -1;
	}
	wait_for_transfer(lowest_index);
	return lowest_index;
}

void process_buffer(int buffer_id)
{
	static int rx_counter = 0;

	buffers_status[buffer_id] = 0; // set status free

	if (channel_buffers[buffer_id].status != PROXY_NO_ERROR)
	{
		printf("transfer error! on buffer %d\n", buffer_id);
		return;
	}

	// Verify the data received matches what was sent (tx is looped back to tx)
	// A unique value in the buffers is used across all transfers
	if (write_rx_files)
	{
		char filename[64];
		snprintf(filename, sizeof(filename), "rx_%u.bin", rx_counter);
		printf("write to file %s... ", filename);
		write_buffer_to_file(filename, channel_buffers[buffer_id].buffer, transfer_size);
		printf("done\n");
	}

	rx_counter++;
}

void transfer_rx()
{
	int in_progress_count = 0;

	// Clear all buffer statuses
	printf("Clear all buffer statuses\n");
	for (int i = 0; i < BUFFER_COUNT; i++)
	{
		buffers_status[i] = 0;
	}

	// Initiate all transfers (waiting when no buffer is available)
	printf("Initiate all transfers (waiting when no buffer is available)\n");
	int transfer_id = 0;
	while (transfer_id < num_transfers)
	{
		int started = start_transfer_maybe();
		if (started == -1)
		{ // no additional buffers available. wait...
			int finished_buffer_id = wait_for_first_transfer();
			process_buffer(finished_buffer_id);
		}
		else
		{
			transfer_id++;
		}
	}

	// Finish pending transfers
	printf("Finish pending transfers\n");
	while (true)
	{
		int finished_buffer_id = wait_for_first_transfer();
		if (finished_buffer_id == -1)
		{
			break;
		}
		process_buffer(finished_buffer_id);
	}
}

// The main program starts the transmit thread and then does the receive processing to do a number of DMA transfers.
int main(int argc, char *argv[])
{
	if (argc != 3 && argc != 4)
	{
		printf("Usage: %s <# of DMA transfers to perform> <# of bytes in each transfer in KB (< 1MB)> "
		       "[write rx file, 0 or 1]\n",
		       argv[0]);
		exit(EXIT_FAILURE);
	}

	// Get the number of transfers to perform
	num_transfers = atoi(argv[1]);
	printf("num_transfers = %d\n", num_transfers);

	// Get the size of the test to run, making sure it's not bigger than the size of the buffers and
	// convert it from KB to bytes
	transfer_size = atoi(argv[2]);
	if (transfer_size > BUFFER_SIZE || transfer_size < 0)
	{
		transfer_size = BUFFER_SIZE;
	}
	transfer_size *= 1024;
	printf("transfer_size = %u\n", transfer_size);

	if (argc == 4)
	{
		write_rx_files = (atoi(argv[3]) != 0);
	}

	fd = open(rx_channel_path, O_RDWR);
	if (fd < 1)
	{
		printf("Unable to open DMA proxy device file: %s\n", rx_channel_path);
		exit(EXIT_FAILURE);
	}

	channel_buffers = (struct channel_buffer *)mmap(NULL,                                            // req addr
	                                                sizeof(struct channel_buffer) * RX_BUFFER_COUNT, // length
	                                                PROT_READ | PROT_WRITE,                          // protection
	                                                MAP_SHARED,                                      // flags
	                                                fd,                                              // fd
	                                                0                                                // offset
	);
	if (channel_buffers == MAP_FAILED)
	{
		printf("failed\n");
		exit(EXIT_FAILURE);
	}

	transfer_rx();

	// Unmap rx channel
	munmap(channel_buffers, sizeof(struct channel_buffer));
	close(fd);

	return 0;
}
