#!/bin/bash
startPath="$PWD"
set -e

if [[ ! -z "$ADD" ]]; then
	for subdir in $(dirname $(find sources -maxdepth 7 -name .git)); do
		echo "* $subdir"

		# get the remote url for each submodule
		pushd $subdir > /dev/null
		fetchUrl=$(git remote -v | grep origin | awk '/fetch/ {print $2}' | head -n 1)
		popd > /dev/null

		# for my purposes repos without remotes are useless
		# but you may have a different use case
		if [[ -z $fetchUrl ]]; then
			echo "     Fetch URL not found"
			continue
		else
			echo "     Fetch URL is: $fetchUrl"
		fi


		# make sure it isn't tracked as a submodule already
		# set +e
		# git submodule --quiet status $subdir 2> /dev/null
		# set -e
		# status=$?

		# if [[ $status -eq 0 ]]; then
		# 	echo "     Already a submodule"
		# else
		# 	# if it doesn't exist yet then create it
		# 	echo "     Adding as submodule"
		set +e
		git submodule add $fetchUrl $subdir
		set -e
		# fi
	done
fi

if [[ ! -z "$REMOTES" ]]; then
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
fi

