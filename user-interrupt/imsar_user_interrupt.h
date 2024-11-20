#ifndef __IMSAR_USER_INTERRUPT_H
#define __IMSAR_USER_INTERRUPT_H

// macros (these intentionally use the same values as intc -- to stay compatible)
#define IMSAR_USER_INTERRUPT_IOCTL_BASE 'W'
#define IMSAR_USER_INTERRUPT_IOCTL_SET_TIMEOUT_MS _IO(IMSAR_USER_INTERRUPT_IOCTL_BASE, 6)
#define IMSAR_USER_INTERRUPT_IOCTL_SET_BEHAVIOR _IO(IMSAR_USER_INTERRUPT_IOCTL_BASE, 7)


enum imsar_user_interrupt_behavior
{
	// read() and poll() return only on the next interrupt (default; no missed interrupt handling)
	// Note: Only compatible with blocking read() calls
	IMSAR_USER_INTERRUPT_BEHAVIOR_NEXT_ONLY = 0,

	// read() and poll() return if an interrupt has occurred since the last call or on the next interrupt
	IMSAR_USER_INTERRUPT_BEHAVIOR_NEXT_OR_MISSED = 1,
};

#endif
