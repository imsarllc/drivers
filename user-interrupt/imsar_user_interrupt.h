#ifndef __IMSAR_USER_INTERRUPT_H
#define __IMSAR_USER_INTERRUPT_H

// macros (these intentionally use the same values as intc -- to stay compatible)
#define IMSAR_USER_INTERRUPT_IOCTL_BASE 'W'
#define IMSAR_USER_INTERRUPT_IOCTL_TIMEOUT _IO(IMSAR_USER_INTERRUPT_IOCTL_BASE, 6)

#endif
