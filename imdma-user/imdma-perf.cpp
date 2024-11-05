// IMSAR DMA performance test

extern "C"
{
#include "libimdma.h"
}

#include <signal.h>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <queue>
#include <sstream>

static bool start_transfer(imdma_transfer_t *dmaTransfer, unsigned int lengthBytes)
{
	// std::cout << "Starting transfer" << std::endl;

	int setLenRc = imdma_transfer_set_length(dmaTransfer, lengthBytes);
	if (setLenRc != 0)
	{
		fprintf(stderr, "failed to set transfer buffer length\n");
		return false;
	}

	// Request a single transfer
	int startRc = imdma_transfer_start_async(dmaTransfer);
	if (startRc != 0)
	{
		fprintf(stderr, "failed to start transfer\n");
		return false;
	}

	int timeoutRc = imdma_transfer_set_timeout_ms(dmaTransfer, 3000);
	if (timeoutRc != 0)
	{
		fprintf(stderr, "failed to set transfer timeout\n");
		return false;
	}

	return true;
}

static unsigned int finish_transfer(imdma_transfer_t *dmaTransfer)
{
	// std::cout << "Waiting to finish transfer" << std::endl;

	imdma_transfer_finish_async(dmaTransfer);

	const void *dmaBuffer = imdma_transfer_get_data_const(dmaTransfer);
	unsigned int dmaBufferLen = imdma_transfer_get_length(dmaTransfer);

	imdma_transfer_free(dmaTransfer);

	if (dmaBuffer == NULL || dmaBufferLen == 0)
	{
		fprintf(stderr, "bad transfer result\n");
		return 0;
	}

	return dmaBufferLen;
}

volatile bool running = true;

static void ctrlc(int sig)
{
	running = false;
	signal(SIGINT, SIG_DFL);
}

int main(int argc, const char *const argv[])
{
	signal(SIGINT, ctrlc);

	if (argc < 2)
	{
		std::cout << "Usage: " << argv[0] << " <device> [lengthBytes:1000] [seconds:0]\n";
		std::cout << "Example: " << argv[0] << " /dev/imdma_downsampled\n";
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

	unsigned int seconds = 0;
	if (argc >= 4)
	{
		seconds = strtoul(argv[3], NULL, 10);
	}

	std::queue<imdma_transfer_t *> pendingTransfers;

	std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point stopTime = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);

	std::chrono::steady_clock::time_point nextReportTime =
	    std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
	unsigned long bytesPerSecond = 0;
	unsigned long totalBytes = 0;
	while (running)
	{
		imdma_transfer_t *dmaTransfer = imdma_transfer_alloc(imdma);
		if (dmaTransfer != NULL)
		{
			bool started = start_transfer(dmaTransfer, lengthBytes);
			if (!started)
			{
				break;
			}
			pendingTransfers.push(dmaTransfer);
		}
		else
		{
			imdma_transfer_t *finishedTransfer = pendingTransfers.front();
			unsigned int transferredBytes = finish_transfer(finishedTransfer);
			bytesPerSecond += transferredBytes;
			totalBytes += transferredBytes;
			pendingTransfers.pop();
		}

		auto now = std::chrono::steady_clock::now();
		if (now > nextReportTime)
		{
			float mbitsPerSec = (static_cast<float>(bytesPerSecond) * 8 / 1000 / 1000);
			std::cout << std::fixed << std::setprecision(1) << mbitsPerSec << " Mbit/s" << std::endl;
			bytesPerSecond = 0;
			nextReportTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
		}

		if (seconds != 0 && now > stopTime)
		{
			break;
		}
	}

	while (pendingTransfers.size())
	{
		imdma_transfer_t *finishedTransfer = pendingTransfers.front();
		unsigned int transferredBytes = finish_transfer(finishedTransfer);
		bytesPerSecond += transferredBytes;
		totalBytes += transferredBytes;
		pendingTransfers.pop();
	}

	std::chrono::steady_clock::time_point endTime = std::chrono::steady_clock::now();

	double durationSecs = std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime).count();

	float mbitsPerSec = (static_cast<float>(totalBytes) * 8 / 1000 / 1000) / durationSecs;

	// std::cout << totalBytes << " bytes" << std::endl;
	// std::cout << durationSecs << " seconds" << std::endl;
	std::cout << mbitsPerSec << " avg Mbits/s" << std::endl;

	imdma_free(imdma);

	return 0;
}
