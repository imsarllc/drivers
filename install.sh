#!/usr/bin/env bash

HOSTNAME=${1:?hostname required}
DST=/boot/imsar/

SRC_KERNEL=sources/kernel/kernel-5.10
SRC_BOOT=$SRC_KERNEL/arch/arm64/boot

SRC_IMAGE=$SRC_BOOT/Image
SRC_DTB=$SRC_BOOT/dts/nvidia/tegra194-p2888-0001-p2822-0000.dtb

scp $SRC_IMAGE $SRC_DTB $HOSTNAME:$DST
