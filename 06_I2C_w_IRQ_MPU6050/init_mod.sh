#!/bin/bash

# set -euo pipefail makes the script exit on errors, unset variables, 
# and failed pipelines for safer scripting.
set -euo pipefail

MODULE_KO="mpu6050"    # ko filename without .ko
DEV_NODE="/dev/mpu6050"  # matches DEV_NAME in driver
WAIT_SECS=5

# shopt -s nullglob makes globs(e.g. *.txt) that don't match any files
# expand to nothing instead of the pattern.
shopt -s nullglob

remove_overlays() {
    local removed=0
    for d in /sys/kernel/config/device-tree/overlays/*${MODULE_KO}-overlay; do
        [ -e "$d" ] || continue
        name=$(basename "$d")
        echo "🗑️  Removing applied overlay $name..."
        if sudo rmdir "$d" 2>/dev/null; then
            echo "✅ Removed $name"
        else
            echo "⚠️  rmdir failed for $name; attempting graceful disable..."
            sudo sh -c 'echo 0 > "'"$d"'/status"' 2>/dev/null || true
            sleep 0.1
            sudo rmdir "$d" 2>/dev/null || true
        fi
        removed=1
    done
    if [ "$removed" -eq 0 ]; then
        echo "ℹ️  Overlay ${MODULE_KO}-overlay not currently applied."
    fi
}

mount_module() {
    echo "🔧 Building module..."
    make

    # remove any existing overlays that match the pattern
    remove_overlays

    # copy dtbo if present
    if [ -f "${MODULE_KO}-overlay.dtbo" ]; then
        echo "📁 Copying ${MODULE_KO}-overlay.dtbo to /boot/overlays/"
        sudo cp "${MODULE_KO}-overlay.dtbo" /boot/overlays/
    else
        echo "⚠️  ${MODULE_KO}-overlay.dtbo not found; build step may have failed."
    fi

    echo "🧩 Applying overlay ${MODULE_KO}-overlay"
    if ! sudo dtoverlay -v "${MODULE_KO}-overlay"; then
        echo "❌ dtoverlay failed. Kernel log (last 40 lines):"
        dmesg | tail -n 40
        return 1
    fi

    # reload module
    if lsmod | awk '{print $1}' | grep -xq "$MODULE_KO"; then
        echo "📦 Removing existing module $MODULE_KO..."
        sudo rmmod "$MODULE_KO" || true
    fi

    echo "📥 Inserting module ${MODULE_KO}.ko ..."
    if ! sudo insmod "${MODULE_KO}.ko"; then
        echo "❌ insmod failed"
        return 1
    fi

    sudo udevadm settle --timeout=3 || true

    for i in $(seq 1 $WAIT_SECS); do
        if [ -e "$DEV_NODE" ]; then
            echo "✅ Device node ready: $DEV_NODE"
            return 0
        fi
        sleep 1
    done

    echo "⚠️  $DEV_NODE not found after ${WAIT_SECS}s."
    return 1
}

clean_module() {
    echo "🧹 Cleaning module and device..."

    if lsmod | awk '{print $1}' | grep -xq "$MODULE_KO"; then
        echo "📦 Removing module $MODULE_KO..."
        sudo rmmod "$MODULE_KO" || true
    else
        echo "ℹ️  Module $MODULE_KO not loaded."
    fi

    if [ -e "$DEV_NODE" ]; then
        echo "⚠️  Removing leftover device node $DEV_NODE..."
        sudo fuser -k "$DEV_NODE" || true
        sleep 0.2
        sudo rm -f "$DEV_NODE" || true
    fi

    remove_overlays

    if [ -f "/boot/overlays/${MODULE_KO}-overlay.dtbo" ]; then
        echo "🗑️  Removing /boot/overlays/${MODULE_KO}-overlay.dtbo..."
        sudo rm -f "/boot/overlays/${MODULE_KO}-overlay.dtbo"
    else
        echo "ℹ️  No dtbo file in /boot/overlays/."
    fi

    echo "✅ Cleanup complete."
}

case "${1:-}" in
    mount) mount_module ;;
    clean) clean_module ;;
    *) echo "Usage: $0 {mount|clean}"; exit 1 ;;
esac
