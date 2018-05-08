#~/bin/sh -ex
VERSION=4.6.0-xilinx-g98937f2f11ab

pwd
echo 'Copying uImage to sdcard'
mount /dev/mmcblk0p1 /boot
cp /lib/modules/$VERSION/uImage /boot
umount /boot

echo 'A reboot is required'
