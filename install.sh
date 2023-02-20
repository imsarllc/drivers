#!/usr/bin/env bash

OP=$1
HOSTNAME=${2:?hostname required}

SRC_KERNEL=sources/kernel/kernel-5.10
BOOT_DST=/boot/imsar/

case $OP in
    "kernel")
        SRC_BOOT=$SRC_KERNEL/arch/arm64/boot

        SRC_IMAGE=$SRC_BOOT/Image
        SRC_DTB=$SRC_BOOT/dts/nvidia/tegra194-p2888-0001-p2822-0000.dtb

        scp $SRC_IMAGE $SRC_DTB $HOSTNAME:$BOOT_DST
        ;;

    "drivers")
        DRIVER_DST=/usr/lib/modules/${kernel_version}/kernel/drivers/imsar/

        scp drivers/*/*.ko $HOSTNAME:$DRIVER_DST
        ssh $HOSTNAME depmod -a
        ;;

    *)
        echo "Operation unrecognized: $OP"
        exit 1
        ;;
esac
