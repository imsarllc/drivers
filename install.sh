#!/usr/bin/env bash

if [[ $# -lt 2 ]]; then
    echo "Usage: $0 <kernel|drivers> <rootfs|hostname>"
    echo "Example for rootfs: $0 kernel rootfs"
    echo "Example for remote host: $0 kernel xavier2"
    exit 1
fi

# colors
cred="\033[31m"
cgreen="\033[32m"
clgreen="\033[36m"
clblue="\033[34m"
creset="\033[m"

heading() {
    echo -e "${clgreen}$1${creset}"
}

section() {
    echo -e "${clblue}$1${creset}"
}

KERNEL_VERSION=5.10.104-tegra

OP=$1
DEST=${2:?destination required}

ROOTFS=0
if [[ "$DEST" == "rootfs" ]]; then
    heading "Installing to ./rootfs folder for flashing"
    ROOTFS=1
    DEST="$(realpath rootfs)"
else
    heading "Installing to remote system"
    DEST="root@$DEST"
fi

SRC_KERNEL=sources/kernel/kernel-5.10
BOOT_DST=/boot/imsar/

MOD_DIR=/usr/lib/modules/${KERNEL_VERSION}
DRIVERS_DIR=$MOD_DIR/kernel/drivers

case $OP in
    "kernel")
        # Image and Device Tree
        SRC_BOOT=$SRC_KERNEL/arch/arm64/boot
        SRC_IMAGE=$SRC_BOOT/Image
        SRC_DTB=$SRC_BOOT/dts/nvidia/tegra194-p2888-0001-p2822-0000.dtb

        if [[ $ROOTFS -eq 0 ]]; then
            remote_version=$(ssh $DEST uname -r)
            if [[ "$remote_version" == "$KERNEL_VERSION" ]]; then
                echo "Compiled Kernel Version: $KERNEL_VERSION"
                echo "Remote Kernel Version: $remote_version"
                echo -e "${cred}"
                echo "Kernel modules cannot be replaced while the kernel is running."
                echo "Use rootfs destination and ./flash.sh instead."
                echo -e "${creset}"
                exit 1
            fi
        fi

        if [[ $ROOTFS -eq 1 ]]; then
            section "Copying Image and DTB to rootfs"
            sudo cp -f \
                $SRC_IMAGE \
                $SRC_DTB \
                $DEST$BOOT_DST
        else
            section "Copying Image and DTB to remote host"
            scp \
                $SRC_IMAGE \
                $SRC_DTB \
                $DEST:$BOOT_DST
        fi

        # Kernel modules
        if [[ $ROOTFS -eq 1 ]]; then
            section "Copying kernel modules to rootfs"
            sudo cp -Rf \
                sources/modules/lib/modules/${KERNEL_VERSION}/* \
                $DEST/usr/lib/modules/${KERNEL_VERSION}

            section "Running depmod on rootfs"
            sudo depmod -b $DEST -a ${KERNEL_VERSION}

            # This file is used by flash.sh for the kernel modules
            section "Creating kernel_supplements.tgz2 for flashing rootfs"
            sudo rm -f kernel/kernel_supplements-new.tbz2
            sudo tar --owner root --group root -cjf kernel/kernel_supplements-new.tbz2 -C $DEST lib/modules/${KERNEL_VERSION}
            sudo cp kernel/kernel_supplements{,-$(date +%Y%m%d%H%M)}.tbz2
            sudo cp kernel/kernel_supplements{-new,}.tbz2
        else
            section "Copying kernel modules to remote host"
            sudo rsync -a --progress \
                sources/modules/lib/modules/${KERNEL_VERSION}/* \
                $DEST:$MOD_DIR

            section "Running depmod on remote host"
            sudo ssh $DEST depmod -a ${KERNEL_VERSION}
        fi

        echo -e "${cgreen}Success${creset}"
        ;;

    "drivers")
        IMSAR_DIR=${DRIVERS_DIR}/imsar/
        moduleFiles=$(find drivers -name '*.ko')
        if [[ $ROOTFS -eq 1 ]]; then
            section "Copying imsar drivers to rootfs"
            sudo mkdir -p $DEST$IMSAR_DIR
            sudo cp -f $moduleFiles $DEST$IMSAR_DIR
            sudo depmod -b $DEST -a ${KERNEL_VERSION}
        else
            section "Copying imsar drivers to remote host"
            ssh $DEST bash -c "cd $DRIVERS_DIR && mkdir -p $IMSAR_DIR"
            scp $moduleFiles $DEST:$IMSAR_DIR

            section "Running depmod on remote host"
            ssh $DEST depmod -a ${KERNEL_VERSION}
        fi

        echo -e "${cgreen}Success${creset}"
        ;;

    *)
        echo -e "${cred}Operation unrecognized: $OP${creset}"
        exit 1
        ;;
esac

