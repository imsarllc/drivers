#!/bin/sh -e
if [ -z "$VIVADO" ]; then
  echo "Error: VIVADO is empty"
  false
fi
if [ -z "$KDIR" ]; then
  echo "Error: KDIR is empty"
  false
fi

if [ $VIVADO == '2013.4' ]; then
  MDST=usr/lib/modules
else
  MDST=lib/modules
fi
KREL=$(cat $KDIR/include/config/kernel.release)
VERSION=$(cat $KDIR/include/config/kernel.release | grep -o -P '\d\.\d')
BUILD_NUMBER=${BUILD_NUMBER:-0}

mkdir -p    build/$VIVADO/etc/udev/rules.d/
cp */*rules build/$VIVADO/etc/udev/rules.d/

DRIVERS="$(find -maxdepth 2 -name '*.ko')"
mkdir -p    build/$VIVADO/$MDST/$KREL/kernel/drivers/imsar
cp $DRIVERS build/$VIVADO/$MDST/$KREL/kernel/drivers/imsar

rsync -a $KDIR/arch/arm/boot/uImage                  build/$VIVADO/$MDST/$KREL/
rsync -a $KDIR/usr/lib/modules/$KREL/kernel          build/$VIVADO/$MDST/$KREL/
rsync -a $KDIR/usr/lib/modules/$KREL/modules.order   build/$VIVADO/$MDST/$KREL/
rsync -a $KDIR/usr/lib/modules/$KREL/modules.builtin build/$VIVADO/$MDST/$KREL/

if [ $VIVADO != '2013.4' ]; then
  /sbin/depmod -a -b build/$VIVADO $KREL
fi

sed "s/VERSION=.*/VERSION=$KREL/" post_install_template.sh > build/post_install.sh

cd build

if [ $VIVADO == '2013.4' ]; then
  mkdir -p drivers/2013.4
  cp ../*/*.ko drivers/2013.4
  cp ../*/*rules drivers
  tar -czf armhf_drivers.tgz -C drivers .
  cat ../installer.sh armhf_drivers.tgz > armhf_drivers_installer.sh
  chmod +x armhf_drivers_installer.sh

  cp post_install.sh $VIVADO/usr/lib/modules/
  tar -czf kernel_modules_$KREL.tgz -C $VIVADO/ .
else
  mkdir -p $VIVADO/etc/modules-load.d/
  echo sarspi > $VIVADO/etc/modules-load.d/sarspi.conf
  echo newhaven_lcd > $VIVADO/etc/modules-load.d/lcd.conf
  fpm --post-install post_install.sh  \
    --output-type deb \
    --description 'Linux kernel and modules for a Zynq based Nanosar C system' \
    --license 'GPL' \
    -m 'IMSAR FPGA Team <fpga@imsar.com>' \
    --vendor 'IMSAR LLC' \
    --url 'https://www.imsar.com/' \
    --name grizzly-kernel \
    -C $VIVADO \
    --architecture armhf \
    --version $VERSION \
    --iteration $BUILD_NUMBER \
    --force  \
    --input-type dir .

  ln grizzly-kernel_${VERSION}-${BUILD_NUMBER}_armhf.deb grizzly-kernel_${VERSION}-latest_armhf.deb
fi
