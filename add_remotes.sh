#!/bin/bash
startPath="$PWD"
set -e

# Drivers
pushd drivers
git remote set-url --push origin git@github.com:imsarllc/drivers.git
popd

# Add kernel-5.10 remotes
pushd sources/kernel/kernel-5.10/
set +e
git remote add adi https://github.com/analogdevicesinc/linux.git
git remote add imsar https://github.com/imsarllc/linux-xlnx.git
git remote set-url --push imsar git@github.com:imsarllc/linux-xlnx.git
git remote add nvidia git://nv-tegra.nvidia.com/linux-5.10.git
git remote add xilinx https://github.com/Xilinx/linux-xlnx.git
set -e
git fetch --all
popd

# Add galen (preferrably we don't do this)
pushd sources/hardware/nvidia/platform/t19x/galen/kernel-dts
git remote add origin git@gitlabee.imsar.us:firmware/stardust-dts.git
git remote add nvidia git://nv-tegra.nvidia.com/device/hardware/nvidia/platform/t19x/stardust-dts.git
# popd

