#!/bin/bash

# Copyright (c) 2012-2022 NVIDIA CORPORATION.  All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#  * Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
#  * Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#  * Neither the name of NVIDIA CORPORATION nor the names of its
#    contributors may be used to endorse or promote products derived
#    from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
# PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
# OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

# verify that git is installed
if  ! which git > /dev/null  ; then
  echo "ERROR: git is not installed. If your linux distro is 10.04 or later,"
  echo "git can be installed by 'sudo apt-get install git-core'."
  exit 1
fi

# source dir
LDK_DIR=$(cd `dirname $0` && pwd)
LDK_DIR="${LDK_DIR}/sources"
# script name
SCRIPT_NAME=`basename $0`
# info about sources.
# NOTE: *Add only kernel repos here. Add new repos separately below. Keep related repos together*
# NOTE: nvethrnetrm.git should be listed after "linux-nvidia.git" due to nesting of sync path
##########################################
# IMSAR changes below:
#  - use '|' instead of ':' as field separator
#  - prepend with the protocol (instead of assuming everything uses git://)
#  - switch kernel-5.10 to imsarllc fork
#  - switch stardust-dts to imsarllc fork
##########################################
NVIDIA_TAG=jetson_35.2.1
IMSAR_TAG=imsar_r35.2.1

SOURCES=()
SOURCES+=("kernel/kernel-5.10|https://github.com/imsarllc/linux-xlnx.git|$IMSAR_TAG")
SOURCES+=("kernel/nvgpu|git://nv-tegra.nvidia.com/linux-nvgpu.git|$NVIDIA_TAG")
SOURCES+=("kernel/nvidia|git://nv-tegra.nvidia.com/linux-nvidia.git|$NVIDIA_TAG")
SOURCES+=("kernel/nvidia/drivers/net/ethernet/nvidia/nvethernet/nvethernetrm|git://nv-tegra.nvidia.com/kernel/nvethernetrm.git|$NVIDIA_TAG")
SOURCES+=("hardware/nvidia/platform/tegra/common|git://nv-tegra.nvidia.com/device/hardware/nvidia/platform/tegra/common.git|$NVIDIA_TAG")
SOURCES+=("hardware/nvidia/soc/t19x|git://nv-tegra.nvidia.com/device/hardware/nvidia/soc/t19x.git|$NVIDIA_TAG")
SOURCES+=("hardware/nvidia/platform/t19x/common|git://nv-tegra.nvidia.com/device/hardware/nvidia/platform/t19x/common.git|$NVIDIA_TAG")
SOURCES+=("hardware/nvidia/platform/t19x/galen/kernel-dts|git@gitlabee.imsar.us:firmware/stardust-dts.git|$IMSAR_TAG")
SOURCES+=("hardware/nvidia/platform/t19x/jakku/kernel-dts|git://nv-tegra.nvidia.com/device/hardware/nvidia/platform/t19x/jakku-dts.git|$NVIDIA_TAG")
SOURCES+=("hardware/nvidia/platform/t19x/mccoy/kernel-dts|git://nv-tegra.nvidia.com/device/hardware/nvidia/platform/t19x/mccoy-dts.git|$NVIDIA_TAG")
SOURCES+=("hardware/nvidia/platform/t19x/galen-industrial/kernel-dts|git://nv-tegra.nvidia.com/device/hardware/nvidia/platform/t19x/galen-industrial-dts.git|$NVIDIA_TAG")
SOURCES+=("hardware/nvidia/platform/t23x/common/kernel-dts|git://nv-tegra.nvidia.com/device/hardware/nvidia/platform/t23x/common-dts.git|$NVIDIA_TAG")
SOURCES+=("hardware/nvidia/platform/t23x/p3768/kernel-dts|git://nv-tegra.nvidia.com/device/hardware/nvidia/platform/t23x/p3768-dts.git|$NVIDIA_TAG")
SOURCES+=("hardware/nvidia/platform/t23x/concord/kernel-dts|git://nv-tegra.nvidia.com/device/hardware/nvidia/platform/t23x/concord-dts.git|$NVIDIA_TAG")
# SOURCES+=("hardware/nvidia/platform/t23x/prometheus/kernel-dts|git://nv-tegra.nvidia.com/device/hardware/nvidia/platform/t23x/prometheus-dts|$NVIDIA_TAG")
SOURCES+=("hardware/nvidia/soc/t23x|git://nv-tegra.nvidia.com/device/hardware/nvidia/soc/t23x.git|$NVIDIA_TAG")
SOURCES+=("hardware/nvidia/soc/tegra|git://nv-tegra.nvidia.com/device/hardware/nvidia/soc/tegra.git|$NVIDIA_TAG")
SOURCES+=("tegra/kernel-src/nv-kernel-display-driver|git://nv-tegra.nvidia.com/tegra/kernel-src/nv-kernel-display-driver.git|$NVIDIA_TAG")

function DownloadAndSync {
	local WHAT_SOURCE="$1"
	local LDK_SOURCE_DIR="$2"
	local REPO_URL="$3"
	local TAG="$4"
	local RET=0

	if [ -d "${LDK_SOURCE_DIR}" ]; then
		echo "Directory for $WHAT, ${LDK_SOURCE_DIR}, already exists!"
		pushd "${LDK_SOURCE_DIR}" > /dev/null
		git status 2>&1 >/dev/null
		if [ $? -ne 0 ]; then
			echo "But the directory is not a git repository -- clean it up first"
			echo ""
			echo ""
			popd > /dev/null
			return 1
		fi
		git fetch --all 2>&1 >/dev/null
		popd > /dev/null
	else
		echo "Downloading $WHAT source from $REPO_URL"

		git clone "$REPO_URL" -n ${LDK_SOURCE_DIR} 2>&1 >/dev/null
		if [ $? -ne 0 ]; then
			echo "$2 source sync failed!"
			echo ""
			echo ""
			return 1
		fi

		echo "The default $WHAT source is downloaded in: ${LDK_SOURCE_DIR}"
	fi

	pushd ${LDK_SOURCE_DIR} > /dev/null
	git fetch --all

	echo "Syncing up with tag $TAG..."
	git checkout $TAG
	if [[ $? -eq 0 ]]; then
		echo "$WHAT_SOURCE source sync'ed to tag $TAG successfully."
	else
		pwd
		echo "FAILED: $WHAT_SOURCE source failed to checkout $TAG!!!"
		RET=1
	fi

	popd > /dev/null

	echo ""
	echo ""

	return "$RET"
}

for ((i=0; i < ${#SOURCES[@]}; i++)); do
	SOURCE_LINE="${SOURCES[$i]}"
	WHAT=$(echo "${SOURCE_LINE}" | cut -f 1 -d '|')
	REPO=$(echo "${SOURCE_LINE}" | cut -f 2 -d '|')
	TAG=$(echo "${SOURCE_LINE}" | cut -f 3 -d '|')

	DownloadAndSync "$WHAT" "${LDK_DIR}/${WHAT}" "${REPO}" "${TAG}"
	if [[ $? -ne 0 ]]; then
		exit 1
	fi
done

echo "All sources successfully sync'ed"
