#~/bin/sh -ex
VERSION=

pwd
echo 'Copying uImage to sdcard'
mount /dev/mmcblk0p1 /boot
cp /lib/modules/$VERSION/uImage /boot
umount /boot
echo 'A reboot is required'
