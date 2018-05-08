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

depmod -a -b build/$VIVADO $KREL

if [ $VIVADO == '2016.4' ]; then
  sed -i "s/VERSION=.*/VERSION=$KREL/" post_install.sh
  fpm --post-install post_install.sh  --output-type deb --name grizzly_kernel --prefix lib/modules -C build/$VIVADO/lib/modules --architecture armhf --version 4.6 --iteration $BUILD_NUMBER --force  --input-type dir .
fi

