export PROJECT_ROOT="$(realpath .)"
export KERNEL_SRC_PATH="$(realpath sources/kernel/kernel-5.10)"
export KERNEL_OUT_PATH="$(realpath sources/kernel_out)"
export KERNEL_MOD_PATH="$(realpath sources/modules)"
export INSTALL_MOD_PATH="${KERNEL_MOD_PATH}"

export CROSS_COMPILE_AARCH64_PATH="$(realpath bootlin-toolchain)"
export CROSS_COMPILE_AARCH64="${CROSS_COMPILE_AARCH64_PATH}/bin/aarch64-buildroot-linux-gnu-"
export ARCH=arm64
export CROSS_COMPILE="${CROSS_COMPILE_AARCH64}"

# fFor NVIDIA Open GPU Kernel Module
export TARGET_ARCH=aarch64
export CC="${CROSS_COMPILE_AARCH64}gcc"
export LD="${CROSS_COMPILE_AARCH64}ld"
export AR="${CROSS_COMPILE_AARCH64}ar"
export CXX="${CROSS_COMPILE_AARCH64}g++"
export OBJCOPY="${CROSS_COMPILE_AARCH64}objcopy"
