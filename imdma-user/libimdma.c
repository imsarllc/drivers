#include "libimdma.h"

#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <stdbool.h>
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

#define LIBIMDMA_NAME "libimdma"

struct imdma_internal_buffer_state_st;

typedef struct imdma_internal_st
{
	int devfd;
	pthread_mutex_t mutex;
	struct imdma_buffer_spec bufferSpec;
	unsigned char *buffer;
	unsigned int totalBufferSize;
	struct imdma_internal_buffer_state_st *bufferStates;
} imdma_internal_t;

typedef enum imdma_internal_buffer_status_en
{
	IMDMA_BUFFER_STATUS_FREE = 0,
	IMDMA_BUFFER_STATUS_ALLOCATED = 1,
	IMDMA_BUFFER_STATUS_BUSY = 2,
	IMDMA_BUFFER_STATUS_DONE = 3,
} imdma_internal_buffer_status_t;

typedef struct imdma_internal_buffer_state_st
{
	imdma_internal_t *imdma;
	imdma_internal_buffer_status_t status; // FREE, ALLOCATED, BUSY, or DONE

	unsigned int buffer_index; // for convenience
	void *data_start;
	unsigned int offset_bytes;

	unsigned int timeout_ms;
	unsigned int length_bytes;
} imdma_buffer_state_t;


imdma_t *imdma_create(const char *devicePath)
{
	imdma_internal_t *state = calloc(1, sizeof(imdma_internal_t));

	if (state == NULL)
	{
		perror(LIBIMDMA_NAME ": failed to malloc");
		return NULL;
	}

	// Open the device
	state->devfd = open(devicePath, 0);
	if (state->devfd < 0)
	{
		free(state);
		perror(LIBIMDMA_NAME ": failed to open device");
		return NULL;
	}

	// Read the buffer specifications (count and size)
	int getSpecResult = ioctl(state->devfd, IMDMA_BUFFER_GET_SPEC, &state->bufferSpec);
	if (getSpecResult < 0)
	{
		close(state->devfd);
		free(state);
		perror(LIBIMDMA_NAME ": failed to get buffer specifications");
		return NULL;
	}

	state->bufferStates = calloc(state->bufferSpec.count, sizeof(imdma_buffer_state_t));
	if (state->bufferStates == NULL)
	{
		close(state->devfd);
		free(state);
		perror(LIBIMDMA_NAME ": failed to allocate buffer state memory");
		return NULL;
	}

	// Compute buffer size
	state->totalBufferSize = state->bufferSpec.count * state->bufferSpec.size_bytes;

	// Map the memory into user space
	state->buffer = mmap(NULL,                   // requested address
	                     state->totalBufferSize, // mapped size
	                     PROT_READ,              // protections
	                     MAP_SHARED,             // flags
	                     state->devfd,           // file descriptor
	                     0);                     // offset
	if (state->buffer == MAP_FAILED)
	{
		close(state->devfd);
		free(state);
		perror(LIBIMDMA_NAME ": failed to mmap");
		return NULL;
	}

	for (int i = 0; i < state->bufferSpec.count; i++)
	{
		imdma_buffer_state_t *buffer = &state->bufferStates[i];
		buffer->imdma = state;
		buffer->buffer_index = i;
		buffer->offset_bytes = i * state->bufferSpec.size_bytes;
		buffer->data_start = &buffer->imdma->buffer[buffer->offset_bytes];
		buffer->status = IMDMA_BUFFER_STATUS_FREE;
		buffer->length_bytes = 0;
	}

	return state;
}

void imdma_free(imdma_t *imdma)
{
	imdma_internal_t *state = (imdma_internal_t *)imdma;

	// Unmap rx channel
	munmap(state->buffer, state->totalBufferSize);

	// Free the buffer states memory
	if (state->bufferStates != NULL)
	{
		free(state->bufferStates);
	}

	// Close the device
	close(state->devfd);

	// Free the imdma memory
	free(state);
}

imdma_transfer_t *imdma_transfer_alloc(imdma_t *imdma)
{
	imdma_internal_t *state = (imdma_internal_t *)imdma;

	// TODO: handle race conditions

	for (int i = 0; i < state->bufferSpec.count; i++)
	{
		imdma_buffer_state_t *buffer = &state->bufferStates[i];
		if (buffer->status == IMDMA_BUFFER_STATUS_FREE)
		{
			buffer->status = IMDMA_BUFFER_STATUS_ALLOCATED;
			return (imdma_transfer_t *)buffer;
		}
	}

	return NULL; // no buffers available
}

int imdma_transfer_start_async(imdma_transfer_t *transfer)
{
	imdma_buffer_state_t *buffer = (imdma_buffer_state_t *)transfer;

	if (buffer->status != IMDMA_BUFFER_STATUS_ALLOCATED)
	{
		return -1;
	}

	// Configure the transfer
	struct imdma_transfer_spec transferSpec = {
	    .buffer_index = buffer->buffer_index, //
	    .length_bytes = buffer->length_bytes  //
	};

	// Start the transfer
	// Note: This ioctl requires transferSpec.buffer_index, transferSpec.length_bytes
	int startResult = ioctl(buffer->imdma->devfd, IMDMA_TRANSFER_START, &transferSpec);
	if (startResult < 0)
	{
		perror(LIBIMDMA_NAME ": failed to start transfer");
		return 1;
	}

	buffer->status = IMDMA_BUFFER_STATUS_BUSY;

	return 0;
}

int imdma_transfer_finish_async(imdma_transfer_t *transfer)
{
	imdma_buffer_state_t *buffer = (imdma_buffer_state_t *)transfer;

	// Configure the transfer
	struct imdma_transfer_spec transferSpec = {
	    .buffer_index = buffer->buffer_index, //
	    .timeout_ms = 1000                    //
	};

	// Wait for transfer to finish
	// Note: This ioctl requires transferSpec.buffer_index
	//       It sets transferSpec.{status, offset_bytes, length_bytes}
	int finishResult = ioctl(buffer->imdma->devfd, IMDMA_TRANSFER_FINISH, &transferSpec);
	if (finishResult < 0)
	{
		perror(LIBIMDMA_NAME ": failed to finish transfer");
		return -1;
	}

	if (transferSpec.status != IMDMA_STATUS_COMPLETE)
	{
		fprintf(stderr, "Transfer failed: status = %u\n", transferSpec.status);
		buffer->length_bytes = 0;
		return -transferSpec.status;
	}

	buffer->length_bytes = transferSpec.length_bytes;
	buffer->status = IMDMA_BUFFER_STATUS_DONE;

	return 0;
}

void imdma_transfer_free(imdma_transfer_t *transfer)
{
	imdma_buffer_state_t *buffer = (imdma_buffer_state_t *)transfer;
	buffer->status = IMDMA_BUFFER_STATUS_FREE;
	buffer->length_bytes = 0;
}

int imdma_transfer_set_length(imdma_transfer_t *transfer, unsigned int lengthBytes)
{
	imdma_buffer_state_t *buffer = (imdma_buffer_state_t *)transfer;
	if (buffer->status != IMDMA_BUFFER_STATUS_ALLOCATED)
	{
		return -1;
	}

	buffer->length_bytes = lengthBytes;

	return 0;
}

unsigned int imdma_transfer_get_length(imdma_transfer_t *transfer)
{
	imdma_buffer_state_t *buffer = (imdma_buffer_state_t *)transfer;
	if (buffer->status != IMDMA_BUFFER_STATUS_DONE)
	{
		return 0;
	}
	return buffer->length_bytes;
}

int imdma_transfer_set_timeout_ms(imdma_transfer_t *transfer, unsigned int timeoutMs)
{
	imdma_buffer_state_t *buffer = (imdma_buffer_state_t *)transfer;
	if (buffer->status != IMDMA_BUFFER_STATUS_ALLOCATED && buffer->status != IMDMA_BUFFER_STATUS_BUSY)
	{
		return -1;
	}

	buffer->timeout_ms = timeoutMs;

	return 0;
}

const void *imdma_transfer_get_data_const(imdma_transfer_t *transfer)
{
	imdma_buffer_state_t *buffer = (imdma_buffer_state_t *)transfer;
	if (buffer->status != IMDMA_BUFFER_STATUS_DONE)
	{
		return NULL;
	}
	return buffer->data_start;
}

void *imdma_transfer_get_data(imdma_transfer_t *transfer)
{
	imdma_buffer_state_t *buffer = (imdma_buffer_state_t *)transfer;
	if (buffer->status != IMDMA_BUFFER_STATUS_ALLOCATED)
	{
		return NULL;
	}
	return buffer->data_start;
}
