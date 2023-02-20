KERNEL_PATH=${KERNEL_PATH:?KERNEL_PATH environment variable required}
CROSS_COMPILE=${CROSS_COMPILE:?CROSS_COMPILE environment variable required}
DRIVER=$1

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <all|driver_name> [clean]"
    echo "Example: $0 all"
    echo "Example: $0 uio"
    exit 1
fi

if [[ $DRIVER == "all" ]]; then
    for driver in allocated-gpio intc jtag lcd sarspi uio; do
        $0 $driver $2
    done
    exit
fi

case "$2" in
    clean)
        make -C $KERNEL_PATH M=$PWD/$DRIVER clean
        ;;

    *)
        ./version.sh > $DRIVER/version.h
        make -C $KERNEL_PATH M=$PWD/$DRIVER modules
        ;;
esac
