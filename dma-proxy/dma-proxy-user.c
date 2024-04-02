#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
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

#define TX_CHANNEL_COUNT 0
#define RX_CHANNEL_COUNT 1

const char *tx_channel_names[] = {"dma_proxy_tx"};
const char *rx_channel_names[] = {"dma_proxy_rx"};

struct channel
{
	struct channel_buffer *buf_ptr;
	int fd;
	pthread_t tid;
};

static bool write_rx_file = false;
static int transfer_size = 0;
static volatile int stop = 0;
int num_transfers = 0;

struct channel tx_channels[TX_CHANNEL_COUNT];
struct channel rx_channels[RX_CHANNEL_COUNT];

void sigint(int a)
{
	stop = 1;
}

static int inc_wrap(int current, int add, int wrap)
{
	return (current + add) % wrap;
}

void write_buffer(const char *file_name, void *buffer, unsigned int length)
{
	printf("write_buffer: opening %s\n", file_name);
	int bin_fd = open(file_name, O_CREAT | O_TRUNC | O_RDWR);
	if (bin_fd == -1)
	{
		printf("write_buffer: failed to open %s\n", file_name);
		return;
	}
	printf("write_buffer: writing %u bytes to %s\n", length, file_name);
	int write_len = write(bin_fd, (void *)buffer, length);
	if (write_len != length)
	{
		printf("write_buffer: failed to write %u bytes (ret was %i)\n", length, write_len);
	}
	printf("write_buffer: closing %s\n", file_name);
	close(bin_fd);
}

void tx_thread(struct channel *channel_ptr)
{
	int in_progress_count = 0;

	// Start all buffers being sent
	for (int buffer_id = 0; buffer_id < TX_BUFFER_COUNT; buffer_id += BUFFER_INCREMENT)
	{
		// Set up the length for the DMA transfer and initialize the transmit buffer to a known pattern.
		channel_ptr->buf_ptr[buffer_id].length = transfer_size;

		// TODO: initialize buffer

		// Start the DMA transfer and this call is non-blocking
		ioctl(channel_ptr->fd, START_XFER, &buffer_id);

		// Keep track of the number of transfers that are in progress and if the number is less
		// than the number of channel buffers then stop before all channel buffers are used
		if (++in_progress_count >= num_transfers)
		{
			break;
		}
	}

	// Start finishing up the DMA transfers that were started beginning with the 1st channel buffer.
	int buffer_id = 0;

	int counter = 0;
	bool stop_in_progress = false;
	while (1)
	{
		// Perform the DMA transfer and check the status after it completes
		// as the call blocks til the transfer is done.
		ioctl(channel_ptr->fd, FINISH_XFER, &buffer_id);
		if (channel_ptr->buf_ptr[buffer_id].status != PROXY_NO_ERROR)
		{
			printf("tx: transfer error\n");
		}

		// Keep track of how many transfers are in progress and how many completed
		in_progress_count--;
		counter++;

		// If all the transfers are done then exit
		if (counter >= num_transfers)
		{
			break;
		}

		// If an early stop (control c or kill) has happened then exit gracefully
		// letting all transfers queued up be completed, but it's trickier because
		// the number of transmit vs receive channel buffers can be very different
		// which means another X transfers need to be done gracefully shutdown the
		// receive without leaving transfers in progress which is unrecoverable
		if (stop & !stop_in_progress)
		{
			stop_in_progress = true;
			num_transfers = counter + RX_BUFFER_COUNT;
		}

		// If the ones in progress will complete the count then don't start more
		if ((counter + in_progress_count) >= num_transfers)
		{
			buffer_id = inc_wrap(buffer_id, BUFFER_INCREMENT, TX_BUFFER_COUNT);
			continue;
		}

		// Initialize the buffer and perform the DMA transfer, check the status after it completes
		// as the call blocks til the transfer is done.
		if (write_rx_file)
		{
			unsigned int *buffer = (unsigned int *)&channel_ptr->buf_ptr[buffer_id].buffer;
			for (int i = 0; i < transfer_size / sizeof(unsigned int); i++)
			{
				buffer[i] = i + ((TX_BUFFER_COUNT / BUFFER_INCREMENT) - 1) + counter;
			}
		}

		// Restart the completed channel buffer to start another transfer and keep
		// track of the number of transfers in progress
		ioctl(channel_ptr->fd, START_XFER, &buffer_id);

		in_progress_count++;

		// Flip to next buffer and wait for it treating them as a circular list
		buffer_id = inc_wrap(buffer_id, BUFFER_INCREMENT, TX_BUFFER_COUNT);
	}
}

void rx_thread(struct channel *channel_ptr)
{
	int in_progress_count = 0;

	// Start all buffers being received
	for (int buffer_id = 0; buffer_id < RX_BUFFER_COUNT; buffer_id += BUFFER_INCREMENT)
	{
		// Set transfer size
		channel_ptr->buf_ptr[buffer_id].length = transfer_size;

		// Clear the buffer
		unsigned int *buffer = channel_ptr->buf_ptr[buffer_id].buffer;
		memset(buffer, 0xff, BUFFER_SIZE);

		// Tell DMA to start transfer
		printf("rx: start xfer for channel 0x%p\n", channel_ptr);
		ioctl(channel_ptr->fd, START_XFER, &buffer_id);

		// Handle the case of a specified number of transfers that is less than the number
		// of buffers
		if (++in_progress_count >= num_transfers)
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
		printf("rx: wait for xfer to finish channel 0x%p\n", channel_ptr);
		ioctl(channel_ptr->fd, FINISH_XFER, &buffer_id);

		if (channel_ptr->buf_ptr[buffer_id].status != PROXY_NO_ERROR)
		{
			printf("rx: transfer error! # transfers %d, # completed %d, # in progress %d\n", num_transfers, rx_counter,
			       in_progress_count);
			break;
		}

		// Verify the data received matches what was sent (tx is looped back to tx)
		// A unique value in the buffers is used across all transfers
		if (write_rx_file)
		{
			unsigned int *buffer = channel_ptr->buf_ptr[buffer_id].buffer;
			write_buffer("dma_rx.bin", buffer, transfer_size);
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
			buffer_id = inc_wrap(buffer_id, BUFFER_INCREMENT, RX_BUFFER_COUNT);
			continue;
		}

		// Start the next buffer again with another transfer keeping track of
		// the number in progress but not finished
		ioctl(channel_ptr->fd, START_XFER, &buffer_id);

		in_progress_count++;

		// Flip to next buffer treating them as a circular list, and possibly skipping some
		// to show the results when prefetching is not happening
		buffer_id = inc_wrap(buffer_id, BUFFER_INCREMENT, RX_BUFFER_COUNT);
	}
}

// Setup the transmit and receive threads so that the transmit thread is low priority to help prevent it from
// overrunning the receive since most testing is done without any backpressure to the transmit channel.
void setup_threads(int *num_transfers)
{
	pthread_attr_t tattr_tx;
	int newprio = 20, i;
	struct sched_param param;

	// The transmit thread should be lower priority than the receive
	// Get the default attributes and scheduling param
	pthread_attr_init(&tattr_tx);
	pthread_attr_getschedparam(&tattr_tx, &param);

	// Set the transmit priority to the lowest
	param.sched_priority = newprio;
	pthread_attr_setschedparam(&tattr_tx, &param);

	for (i = 0; i < RX_CHANNEL_COUNT; i++)
	{
		printf("rx: creating thread for channel %d...", i);
		pthread_create(&rx_channels[i].tid, NULL, (void *)rx_thread, (void *)&rx_channels[i]);
		printf("done\n");
	}

	for (i = 0; i < TX_CHANNEL_COUNT; i++)
	{
		printf("tx: creating thread for channel %d...", i);
		pthread_create(&tx_channels[i].tid, &tattr_tx, (void *)tx_thread, (void *)&tx_channels[i]);
		printf("done\n");
	}
}

// The main program starts the transmit thread and then does the receive processing to do a number of DMA transfers.
int main(int argc, char *argv[])
{
	int buffer_id = 0;
	int max_channel_count = MAX(TX_CHANNEL_COUNT, RX_CHANNEL_COUNT);

	printf("DMA proxy test\n");

	signal(SIGINT, sigint);

	if (argc != 3 && argc != 4)
	{
		printf("Usage: dma-proxy-test <# of DMA transfers to perform> <# of bytes in each transfer in KB (< 1MB)> "
		       "<write rx file, 0 or 1>\n");
		exit(EXIT_FAILURE);
	}

	// Get the number of transfers to perform
	num_transfers = atoi(argv[1]);
	printf("%d transfers\n", num_transfers);

	// Get the size of the test to run, making sure it's not bigger than the size of the buffers and
	// convert it from KB to bytes
	transfer_size = atoi(argv[2]);
	if (transfer_size > BUFFER_SIZE || transfer_size < 0)
	{
		transfer_size = BUFFER_SIZE;
	}
	transfer_size *= 1024;
	printf("%d bytes per transfer\n", transfer_size);

	// Verify is off by default to get pure performance of the DMA transfers without the CPU accessing all the data
	// to slow it down.
	if (argc == 4)
	{
		write_rx_file = atoi(argv[3]) != 0;
	}
	printf("verify = %s\n", write_rx_file ? "yes" : "no");

	// Open the file descriptors for each tx channel and map the kernel driver memory into user space
	for (int i = 0; i < TX_CHANNEL_COUNT; i++)
	{
		char channel_name[64];
		snprintf(channel_name, sizeof(channel_name), "/dev/%s", tx_channel_names[i]);
		printf("tx: opening %s...", channel_name);
		tx_channels[i].fd = open(channel_name, O_RDWR);
		if (tx_channels[i].fd < 1)
		{
			printf("failed\n");
			printf("Unable to open DMA proxy device file: %s\n", channel_name);
			exit(EXIT_FAILURE);
		}
		else
		{
			printf("success\n");
		}
		printf("tx: mmap %s...", channel_name);
		tx_channels[i].buf_ptr =
		    (struct channel_buffer *)mmap(NULL, sizeof(struct channel_buffer) * TX_BUFFER_COUNT, PROT_READ | PROT_WRITE,
		                                  MAP_SHARED, tx_channels[i].fd, 0);
		if (tx_channels[i].buf_ptr == MAP_FAILED)
		{
			printf("failed\n");
			exit(EXIT_FAILURE);
		}
		else
		{
			printf("success at 0x%p\n", (void *)tx_channels[i].buf_ptr);
		}
	}

	// Open the file descriptors for each rx channel and map the kernel driver memory into user space
	for (int i = 0; i < RX_CHANNEL_COUNT; i++)
	{
		char channel_name[64];
		snprintf(channel_name, sizeof(channel_name), "/dev/%s", rx_channel_names[i]);
		printf("rx: opening %s...", channel_name);
		rx_channels[i].fd = open(channel_name, O_RDWR);
		if (rx_channels[i].fd < 1)
		{
			printf("failed\n");
			printf("Unable to open DMA proxy device file: %s\n", channel_name);
			exit(EXIT_FAILURE);
		}
		else
		{
			printf("success\n");
		}
		printf("rx: mmap %s...", channel_name);
		rx_channels[i].buf_ptr =
		    (struct channel_buffer *)mmap(NULL, sizeof(struct channel_buffer) * RX_BUFFER_COUNT, PROT_READ | PROT_WRITE,
		                                  MAP_SHARED, rx_channels[i].fd, 0);
		if (rx_channels[i].buf_ptr == MAP_FAILED)
		{
			printf("failed\n");
			exit(EXIT_FAILURE);
		}
		else
		{
			printf("success at 0x%p\n", (void *)tx_channels[i].buf_ptr);
		}
	}

	// Start the threads & transfers on all channels
	setup_threads(&num_transfers);

	// Do the minimum to know the transfers are done before getting the time for performance
	for (int i = 0; i < RX_CHANNEL_COUNT; i++)
	{
		printf("rx: waiting on thread for channel %d...\n", i);
		pthread_join(rx_channels[i].tid, NULL);
		printf("rx: finished thread for channel %d\n", i);
	}

	printf("stats: Transfer size: %d KB\n", (long long)(num_transfers) * (transfer_size / 1024) * max_channel_count);

	// Clean up all the TX channels
	for (int i = 0; i < TX_CHANNEL_COUNT; i++)
	{
		// Join tx threads
		printf("tx: waiting on thread for channel %d...\n", i);
		pthread_join(tx_channels[i].tid, NULL);
		printf("tx: finished thread for channel %d\n", i);

		// Unmap tx buffers
		printf("tx: munmap channel %d...", i);
		munmap(tx_channels[i].buf_ptr, sizeof(struct channel_buffer));
		printf("done\n");

		// Close tx device
		printf("tx: close channel %d...", i);
		close(tx_channels[i].fd);
		printf("done\n");
	}

	for (int i = 0; i < RX_CHANNEL_COUNT; i++)
	{
		// Unmap rx channels
		printf("rx: munmap channel %d...", i);
		munmap(rx_channels[i].buf_ptr, sizeof(struct channel_buffer));
		printf("done\n");

		// Close rx channels
		printf("rx: close channel %d...", i);
		close(rx_channels[i].fd);
		printf("done\n");
	}

	return 0;
}
