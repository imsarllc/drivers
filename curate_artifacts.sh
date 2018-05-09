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

BUILD_NUMBER=${BUILD_NUMBER:-0}

mkdir -p    build/$VIVADO/etc/udev/rules.d/
cp */*rules build/$VIVADO/etc/udev/rules.d/

mkdir -p               build/$VIVADO/$MDST/$KREL/kernel/drivers/imsar
DRIVERS="$(find -maxdepth 2 -name '*.ko')"
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
  fpm --post-install post_install.sh  --output-type deb --name grizzly_kernel --prefix lib/modules -C $VIVADO/lib/modules --architecture armhf --version 4.6 --iteration $BUILD_NUMBER --force  --input-type dir .
fi
