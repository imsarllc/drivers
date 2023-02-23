Prereqs
=======


APT Packages
------------

```
apt install build-essential
```


NVIDIA packages
---------------

1. Download NVIDIA Jetson Linux support packages from https://developer.nvidia.com/embedded/jetson-linux-r3521:

    * Driver Package (BSP)
    * Sample Root Filesystem
    * Bootlin Toolchain gcc 9.3

2. Extract the archives using the following table:

    ```
    Source Package Name          Archive path        Destination path
    -------------------          ------------        -----------------------
    Driver Package (BSP)         /Linux_for_Tegra    ./
    Sample Root Filesystem       /                   ./rootfs
    Bootlin Toolchain gcc 9.3    /                   ./bootlin-toolchain
    ```

    Example commands:
    ```
    tar xf Jetson_Linux_R35.2.1_aarch64.tbz2 --strip-components=1 Linux_for_Tegra
    sudo tar xpf Tegra_Linux_Sample-Root-Filesystem_R35.2.1_aarch64.tbz2 -C rootfs/
    tar xf aarch64--glibc--stable-final.tar.gz -C bootlin-toolchain
    ```

Setup
=====

1. Add submodules and their remotes:

   ```
   git submodule update --init --recursive
   ./imsar_sources_sync.sh
   ./add_remotes.sh
   ```

2. Install dependencies for flashing:

   ```
   sudo ln -s /usr/bin/python{3,}
   sudo ./apply_binaries.sh
   sudo ./tools/l4t_flash_prerequisites.sh
   sudo ./tools/l4t_create_default_user.sh -u imsar -p <password> -n <hostname> --accept-license
   ```

   NOTE: l4t_flash_prerequisites.sh needs to have `python` changed to `python3`


Build
=====


1. Linux kernel, device tree blobs
    ```
    source imsar_env.sh
    cd sources
    ./imsar_nvbuild.sh -o kernel_out
    ```

2. NVIDIA display drivers
    ```
    source imsar_env.sh

    make -C sources/tegra/kernel-src/nv-kernel-display-driver/NVIDIA-kernel-module-source-TempVersion -j$(nproc) SYSSRC="${KERNEL_SRC_PATH}" SYSOUT="${KERNEL_OUT_PATH}" modules modules_install
    ```

3. IMSAR drivers
    ```
    source imsar_env.sh
    cd drivers
    ./build.sh all
    ```

Install
=======

You can install in two different ways. The first method is to the ./rootfs folder and then use the NVIDIA flash.sh script to completely re-flash the Xavier. Alternatively, you can install it to a remote system via ssh/scp/rsync (with some limitations).

Both methods assume you have or will have a working Xavier booting from `/boot/imsar/Image` with `/boot/imsar/tegra194-p2888-0008-p2822-0000.dtb` as the FDT. This can be
configured under `/boot/extlinux/extlinux.conf`. When using UEFI (which r35.2.1 does
by default), make sure to set the boot order to use eMMC first.


Method 1: Install to ./rootfs and use flash.sh
----------------------------------------------
```
./install.sh kernel rootfs
./install.sh drivers rootfs
sudo ./flash.sh jetson-agx-xavier-devkit internal
```

Method 2: Install to remote system via ssh/scp/rsync
----------------------------------
NOTE: You can only install the kernel to a remote system if it is running a different
kernel version. This is because you cannot install the kernel modules for a kernel that is currently running.

```
./install.sh kernel <hostname>
./install.sh drivers <hostname>
```
