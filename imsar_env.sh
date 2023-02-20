export KERNEL_PATH=$(realpath sources/kernel/kernel-5.10)
export CROSS_COMPILE_AARCH64_PATH=$(realpath bootlin-toolchain)
export CROSS_COMPILE_AARCH64=${CROSS_COMPILE_AARCH64_PATH}/bin/aarch64-buildroot-linux-gnu-
export ARCH=arm64
export CROSS_COMPILE=${CROSS_COMPILE_AARCH64}
