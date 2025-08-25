#!/bin/bash

# set -euo pipefail makes the script exit on errors, unset variables, 
# and failed pipelines for safer scripting.
set -euo pipefail

MODULE_KO="i2c_bmp280"    # ko filename without .ko
DEV_NODE="/dev/${MODULE_KO}"  # matches DEV_NAME in driver
WAIT_SECS=5
OVERLAY="${MODULE_KO}-overlay"

# shopt -s nullglob makes globs(e.g. *.txt) that don't match any files
# expand to nothing instead of the pattern.
shopt -s nullglob

mount_module() {
    echo "🔧 Building module..."
    make
    
    sudo cp "${OVERLAY}.dtbo" /boot/overlays/
    sudo dtoverlay $OVERLAY

    # unload previous module if present
    if lsmod | awk '{print $1}' | grep -xq "$MODULE_KO"; then
        echo "📦 Removing existing module $MODULE_KO..."
        sudo rmmod "$MODULE_KO" || true
    fi

    echo "📥 Inserting module $MODULE_KO.ko ..."
    sudo insmod "${MODULE_KO}.ko" || { echo "❌ insmod failed"; return 1; }

    # give udev a short moment to create the device node
    sudo udevadm settle --timeout=3 || true

    for i in $(seq 1 $WAIT_SECS); do
        if [ -e "$DEV_NODE" ]; then
            echo "✅ Device node ready: $DEV_NODE"
            return 0
        fi
        sleep 1
    done

    echo "⚠️  $DEV_NODE not found after ${WAIT_SECS}s. If using device tree overlay, ensure the DT node exists and udev is running."
    return 1
}

clean_module() {
    echo "🧹 Cleaning module and device..."

    # Remove kernel module if loaded
    if lsmod | awk '{print $1}' | grep -xq "$MODULE_KO"; then
        echo "📦 Removing module $MODULE_KO..."
        sudo rmmod "$MODULE_KO" || true
    else
        echo "ℹ️  Module $MODULE_KO not loaded."
    fi

    # Kill/remove device node if present
    if [ -e "$DEV_NODE" ]; then
        echo "⚠️  Removing leftover device node $DEV_NODE..."
        sudo fuser -k "$DEV_NODE" || true
        sleep 0.2
        sudo rm -f "$DEV_NODE" || true
    fi

    # Remove applied overlay (runtime)
    if grep -q "$OVERLAY" /proc/device-tree/__symbols__/* 2>/dev/null; then
        echo "🗑️  Removing applied overlay $OVERLAY..."
        echo -"$OVERLAY" | sudo tee /sys/kernel/config/device-tree/overlays/$OVERLAY/status >/dev/null || true
        sudo rmdir /sys/kernel/config/device-tree/overlays/$OVERLAY 2>/dev/null || true
    else
        echo "ℹ️  Overlay $OVERLAY not currently applied."
    fi

    # Delete dtbo from /boot/overlays
    if [ -f "/boot/overlays/${OVERLAY}.dtbo" ]; then
        echo "🗑️  Removing /boot/overlays/${OVERLAY}.dtbo..."
        sudo rm -f "/boot/overlays/${OVERLAY}.dtbo"
    else
        echo "ℹ️  No dtbo file in /boot/overlays/."
    fi

    echo "✅ Cleanup complete."
}

case "${1:-}" in
    mount) mount_module ;;
    clean) clean_module ;;
    *) echo "Usage: $0 {mount|clean}"; exit 1;;
esac
