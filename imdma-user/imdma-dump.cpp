// IMSAR DMA utility to dump to file

extern "C"
{
#include "libimdma.h"
}

#include <signal.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <queue>
#include <sstream>
#include <stdexcept>

class Transfer
{
public:
	Transfer(imdma_t *device) : transfer_{imdma_transfer_alloc(device)}
	{
		if (!transfer_)
		{
			throw std::runtime_error("Cannot allocate transfer");
		}
	}
	Transfer(const Transfer &&other) = delete;
	~Transfer() { imdma_transfer_free(transfer_); }

	int setTimeoutMs(unsigned int timeoutMs) { return imdma_transfer_set_timeout_ms(transfer_, timeoutMs); }

	int setLengthBytesIn(unsigned int lengthBytes) { return imdma_transfer_set_length(transfer_, lengthBytes); }

	int startTransferIn(unsigned int lengthBytes)
	{
		setLengthBytesIn(lengthBytes);
		return start();
	}

	int finishTransfer(unsigned int timeoutMs)
	{
		setTimeoutMs(timeoutMs);
		return finish();
	}

	int start() { return imdma_transfer_start_async(transfer_); }
	int finish() { return imdma_transfer_finish(transfer_); }

	const unsigned int getDataLengthIn() { return imdma_transfer_get_length(transfer_); }
	const void *getDataPointerIn() { return imdma_transfer_get_data_const(transfer_); }

private:
	imdma_transfer_t *transfer_;
};

class Device
{
public:
	Device(const char *devicePath) : device_{imdma_create(devicePath)}
	{
		if (!device_)
		{
			throw std::runtime_error("Cannot create device");
		}
	}
	Device(const Device &&other) = delete;
	~Device() { imdma_free(device_); }

	std::unique_ptr<Transfer> createTransfer()
	{
		try
		{
			return std::unique_ptr<Transfer>(new Transfer(device_));
		}
		catch (std::runtime_error &err)
		{
			return nullptr;
		}
	}

private:
	imdma_t *device_;
};

volatile bool running = true;

static void ctrlc(int sig)
{
	running = false;
	signal(SIGINT, SIG_DFL);
}

static bool finishTransfer(const char *filePrefix, unsigned int transferNumber, Transfer &transfer,
                           unsigned int timeoutMs)
{
	int finishStatus = transfer.finishTransfer(timeoutMs);
	if (finishStatus != 0)
	{
		std::cerr << "transfer " << transferNumber << " failed with status " << finishStatus << std::endl;
		return false;
	}
	unsigned int transferredBytes = transfer.getDataLengthIn();
	if (transferredBytes == 0)
	{
		std::cerr << "transfer " << transferNumber << " was empty" << std::endl;
		return false;
	}

	std::stringstream ss;
	ss << filePrefix;
	ss << std::setfill('0');
	ss << std::setw(10);
	ss << transferNumber;
	std::string filename = ss.str();
	std::ofstream file(filename);
	if (!file.is_open())
	{
		std::cerr << "failed to open file \"" << filename << "\"" << std::endl;
		return false;
	}

	file.write(reinterpret_cast<const char *>(transfer.getDataPointerIn()), transfer.getDataLengthIn());

	return true;
}

int main(int argc, const char *const argv[])
{
	if (argc < 3)
	{
		std::cout << "Usage: " << argv[0]
		          << " <device> <filename_prefix> [transfer_count=0] [length_bytes=5242880] [timeout_ms=3000]\n";
		std::cout << "Example: " << argv[0] << " /dev/imdma_downsampled /tmp/data_\n";
		return 1;
	}

	const char *devicePath = argv[1];
	const char *filename = argv[2];
	unsigned int numberOfTransfers = 0; // 0 = unlimited
	unsigned int lengthBytes = 5242880; // 5MB
	unsigned int timeoutMs = 3000;      // 3 seconds

	if (argc >= 4)
	{
		numberOfTransfers = strtoul(argv[3], NULL, 10);
	}

	if (argc >= 5)
	{
		lengthBytes = strtoul(argv[4], NULL, 10);
	}

	if (argc >= 6)
	{
		timeoutMs = strtoul(argv[5], NULL, 10);
	}

	signal(SIGINT, ctrlc);

	Device device(devicePath);

	std::queue<std::unique_ptr<Transfer>> pendingTransfers;

	unsigned int transferStartCount = 0;
	unsigned int transferFinishCount = 0;

	while (running && (numberOfTransfers == 0 || transferStartCount < numberOfTransfers))
	{
		std::unique_ptr<Transfer> transfer = device.createTransfer();
		if (transfer)
		{
			int result = transfer->startTransferIn(lengthBytes);
			if (result != 0)
			{
				std::cerr << "failed to start transfer" << std::endl;
				break;
			}
			pendingTransfers.push(std::move(transfer));
			transferStartCount++;
		}
		else
		{
			std::unique_ptr<Transfer> &transfer = pendingTransfers.front();
			bool status = finishTransfer(filename, transferFinishCount++, *transfer, timeoutMs);
			pendingTransfers.pop();
			if (status == false)
			{
				break;
			}
		}
	}

	// Finishing remaining pending transfers
	while (pendingTransfers.size() > 0)
	{
		std::unique_ptr<Transfer> &transfer = pendingTransfers.front();
		finishTransfer(filename, transferFinishCount++, *transfer, timeoutMs);
		pendingTransfers.pop();
	}

	std::cout << "Completed " << transferStartCount << " transfers" << std::endl;

	return 0;
}
