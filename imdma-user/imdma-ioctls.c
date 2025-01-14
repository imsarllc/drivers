#include <stdio.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include "../imdma/imdma.h"

int main()
{
    printf("IMDMA_BUFFER_GET_SPEC  = %lu\n", IMDMA_BUFFER_GET_SPEC);
    printf("IMDMA_BUFFER_RESERVE = %lu\n", IMDMA_BUFFER_RESERVE);
    printf("IMDMA_TRANSFER_START = %lu\n", IMDMA_TRANSFER_START);
    printf("IMDMA_TRANSFER_FINISH = %lu\n", IMDMA_TRANSFER_FINISH);
    printf("IMDMA_BUFFER_RELEASE = %lu\n", IMDMA_BUFFER_RELEASE);
    return 0;
}