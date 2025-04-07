#ifndef _INTC_H_
#define _INTC_H_

// macros
#define INTC_IOCTL_BASE     'W'
#define INTC_INT_COUNT      _IO(INTC_IOCTL_BASE, 0)
#define INTC_ENABLE         _IO(INTC_IOCTL_BASE, 5)
#define INTC_TIMEOUT        _IO(INTC_IOCTL_BASE, 6)
#define INTC_BEHAVIOR		_IO(INTC_IOCTL_BASE, 7)

enum intc_behavior
{
	// read() and poll() return only on the next interrupt (default; no missed interrupt handling)
	// Note: Only compatible with blocking read() calls
	INTC_BEHAVIOR_NEXT_ONLY = 0,

	// read() and poll() return if an interrupt has occurred since the last call or on the next interrupt
	INTC_BEHAVIOR_NEXT_OR_MISSED = 1,
};

#endif // _INTC_H_
