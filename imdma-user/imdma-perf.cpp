// IMSAR DMA performance test

extern "C"
{
#include "libimdma.h"
}

#include <signal.h>

#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <queue>
#include <sstream>

struct StatisticsRecorder
{
	StatisticsRecorder() : nextPrintTime{std::chrono::steady_clock::now() + std::chrono::seconds(1)} {}

	void start() { startTime = std::chrono::steady_clock::now(); }

	void stop() { stopTime = std::chrono::steady_clock::now(); }

	void addTransfer(unsigned int lengthBytes)
	{
		totalBytes += lengthBytes;
		bytesInLastSecond += lengthBytes;
		totalTransfers += 1;
		transfersInLastSecond += 1;
	}

	void printPeriodic()
	{
		auto now = std::chrono::steady_clock::now();
		if (now < nextPrintTime)
		{
			return;
		}

		std::cout << bytesInLastSecond << " B/s " << transfersInLastSecond << " Blocks/s" << std::endl;

		bytesInLastSecond = 0;
		transfersInLastSecond = 0;

		nextPrintTime = now + std::chrono::seconds(1);
	}

	void printFinal()
	{
		float totalMiB = static_cast<float>(totalBytes) / 1024 / 1024;    // MiB
		float totalMb = static_cast<float>(totalBytes) * 8 / 1000 / 1000; // Mb
		double durationSeconds = std::chrono::duration<double>(stopTime - startTime).count();
		std::cout << "Totals: " << totalBytes << " B " << totalTransfers << " Blocks" << std::endl;
		std::cout << durationSeconds << " seconds" << std::endl;
		std::cout << (totalMiB / durationSeconds) << " MiB/s (" << (totalMb / durationSeconds) << " Mb/s)" << std::endl;
	}

	std::chrono::steady_clock::time_point startTime;
	std::chrono::steady_clock::time_point stopTime;

	unsigned long totalBytes{0};
	unsigned long totalTransfers{0};
	unsigned long bytesInLastSecond{0};
	unsigned long transfersInLastSecond{0};

	std::chrono::steady_clock::time_point nextPrintTime;
};

struct TransferEntry
{
	TransferEntry(imdma_transfer_t *transfer) : transfer{transfer}, startTime{std::chrono::steady_clock::now()} {}
	imdma_transfer_t *transfer;
	std::chrono::steady_clock::time_point startTime;
};

static bool start_transfer(imdma_transfer_t *dmaTransfer, unsigned int lengthBytes, unsigned int timeoutMs)
{
	// std::cout << dmaTransfer << " start" << std::endl;

	int setLenRc = imdma_transfer_set_length(dmaTransfer, lengthBytes);
	if (setLenRc != 0)
	{
		std::cerr << "failed to set transfer buffer length" << std::endl;
		return false;
	}

	// Request a single transfer
	int startRc = imdma_transfer_start_async(dmaTransfer);
	if (startRc != 0)
	{
		std::cerr << "failed to start transfer" << std::endl;
		return false;
	}

	int timeoutRc = imdma_transfer_set_timeout_ms(dmaTransfer, timeoutMs);
	if (timeoutRc != 0)
	{
		std::cerr << "failed to set transfer timeout" << std::endl;
		return false;
	}

	return true;
}

static unsigned int finish_transfer(imdma_transfer_t *dmaTransfer)
{
	// std::cout << dmaTransfer << " wait for finish" << std::endl;

	imdma_transfer_finish(dmaTransfer);

	const void *dmaBuffer = imdma_transfer_get_data_const(dmaTransfer);
	if (dmaBuffer == nullptr)
	{
		std::cerr << "unable to get data buffer" << std::endl;
		return 0;
	}

	unsigned int dmaBufferLen = imdma_transfer_get_length(dmaTransfer);
	if (dmaBufferLen == 0)
	{
		std::cerr << "no data was transferred" << std::endl;
		return 0;
	}

	imdma_transfer_free(dmaTransfer);

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
		std::cout << "Usage: " << argv[0] << " <device> [lengthBytes:1000] [seconds:0] [timeout_ms:3000]\n";
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

	unsigned int timeoutMs = 3000; // 3 seconds
	if (argc >= 5)
	{
		timeoutMs = strtoul(argv[4], NULL, 10);
	}

	std::queue<imdma_transfer_t *> pendingTransfers;

	StatisticsRecorder stats;

	std::chrono::steady_clock::time_point stopTime = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);

	stats.start();

	while (running)
	{
		imdma_transfer_t *dmaTransfer = imdma_transfer_alloc(imdma);
		if (dmaTransfer != nullptr)
		{
			bool started = start_transfer(dmaTransfer, lengthBytes, timeoutMs);
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
			pendingTransfers.pop();
			if (transferredBytes != 0)
			{
				stats.addTransfer(transferredBytes);
			}
		}

		stats.printPeriodic();

		auto now = std::chrono::steady_clock::now();
		if (seconds != 0 && now > stopTime)
		{
			break;
		}
	}

	// Finishing remaining pending transfers
	while (pendingTransfers.size() > 0)
	{
		imdma_transfer_t *finishedTransfer = pendingTransfers.front();
		unsigned int transferredBytes = finish_transfer(finishedTransfer);
		pendingTransfers.pop();
		if (transferredBytes != 0)
		{
			stats.addTransfer(transferredBytes);
		}
		stats.printPeriodic();
	}

	stats.stop();

	stats.printFinal();

	imdma_free(imdma);

	return 0;
}
