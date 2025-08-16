#!/bin/bash
set -e

DEVICE_NAME="gpio_7seg"
MAJOR=138

mount_module() {
    echo "🔧 Compiling kernel module..."
    make

    # Remove device file if exists
    if [ -e /dev/$DEVICE_NAME ]; then
        echo "⚠️  Device /dev/$DEVICE_NAME exists — removing..."
        sudo fuser -k /dev/$DEVICE_NAME || true
        sleep 0.5
        sudo rm -f /dev/$DEVICE_NAME
    fi

    # Remove module if loaded
    if lsmod | grep -q "^$DEVICE_NAME"; then
        echo "📦 Removing module $DEVICE_NAME..."
        sudo rmmod $DEVICE_NAME || true
    fi

    echo "📥 Inserting module with major $MAJOR..."
    sudo insmod $DEVICE_NAME.ko dev_major=$MAJOR

    echo "📄 Creating /dev/$DEVICE_NAME..."
    sudo mknod /dev/$DEVICE_NAME c $MAJOR 0
    sudo chmod 666 /dev/$DEVICE_NAME

    echo "✅ Module $DEVICE_NAME loaded and device created at /dev/$DEVICE_NAME"
}

clean_module() {
    echo "🧹 Cleaning module and device..."
    if [ -e /dev/$DEVICE_NAME ]; then
        sudo fuser -k /dev/$DEVICE_NAME || true
        sleep 0.5
        sudo rm -f /dev/$DEVICE_NAME
    fi

    if lsmod | grep -q "^$DEVICE_NAME"; then
        sudo rmmod $DEVICE_NAME || true
    else
        echo "ℹ️  Module $DEVICE_NAME is not loaded."
    fi

    echo "✅ Cleanup complete."
}

case "$1" in
    mount)
        mount_module
        ;;
    clean)
        clean_module
        ;;
    *)
        echo "Usage: $0 {mount|clean}"
        exit 1
        ;;
esac
