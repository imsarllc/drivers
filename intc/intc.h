#ifndef _INTC_H_
#define _INTC_H_

// macros
#define INTC_IOCTL_BASE     'W'
#define INTC_INT_COUNT      _IO(INTC_IOCTL_BASE, 0)
#define INTC_ENABLE         _IO(INTC_IOCTL_BASE, 5)
#define INTC_TIMEOUT        _IO(INTC_IOCTL_BASE, 6)

#ifndef __KERNEL__
// userspace
#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>

#ifndef ERESTARTSYS
#define ERESTARTSYS 512
#endif // ERESTARTSYS

static inline ssize_t intc_read(int fd, void *buf, size_t bytes)
{
	int res;
	do
		res = read(fd, buf, bytes);
	while(res == -ERESTARTSYS);
	if(res < 0)
		printf("errno:%d\n", errno);
	return res;
}
#endif // __KERNEL__

#endif // _INTC_H_
