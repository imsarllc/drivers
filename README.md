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
    tar xf ${L4T_RELEASE_PACKAGE} --strip-components=1 Linux_for_Tegra
    sudo tar xpf ${SAMPLE_FS_PACKAGE} -C rootfs/
    tar xf ${BOOTLIN_TOOLCHAIN_PACKAGE} -C bootlin-toolchain
    ```

Setup
=====

1. Add submodule remotes:

   ```
   REMOTES=1 ./add_submodules.sh
   ```

2. Install dependencies for flashing (optional):

   ```
   sudo ./apply_binaries.sh
   sudo ./tools/l4t_flash_prerequisites.sh
   ```


Build
=====


Linux kernel, device tree blobs
------------
```
source imsar_env.sh
cd sources
./imsar_nvbuild.sh
```

IMSAR drivers
------------
```
cd drivers
export KERNEL_PATH=$(realpath ../sources/kernel/kernel-5.10)
./build.sh all
```

Install
=======

Linux kernel, device tree blobs
------------
Assumes you have a working Xavier booting from `/boot/imsar/Image` with `/boot/imsar/tegra194-p2888-0008-p2822-0000.dtb` as the FDT
```
./install.sh kernel <hostname>
```


IMSAR drivers
-------------
Assumes kernel version: 5.10.104-tegra
```
./install.sh drivers <hostname>
```

