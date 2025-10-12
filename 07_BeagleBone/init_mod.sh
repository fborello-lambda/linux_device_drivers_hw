#!/bin/bash

# set -euo pipefail makes the script exit on errors, unset variables, 
# and failed pipelines for safer scripting.
set -euo pipefail

MODULE_KO="mpu6050"    # ko filename without .ko
DEV_NODE="/dev/mpu6050"  # matches DEV_NAME in driver
WAIT_SECS=5

# Using U-BOOT -> To load the overlay we need a reboot.
OVERLAYS_DIR="/lib/firmware/"

# shopt -s nullglob makes globs(e.g. *.txt) that don't match any files
# expand to nothing instead of the pattern.
shopt -s nullglob

set_overlay() {
    echo "🔧 Building overlay..."
    make overlay

    local uenv="/boot/uEnv.txt"
    local overlays_dir="${OVERLAYS_DIR:-/lib/firmware/}"
    local dtbo="${overlays_dir}${MODULE_KO}-overlay.dtbo"

    # copy dtbo if present
    if [ -f "${MODULE_KO}-overlay.dtbo" ]; then
        echo "📁 Copying ${MODULE_KO}-overlay.dtbo to ${OVERLAYS_DIR}"
        echo "‼️  If changes were made reboot the system in order to apply the overlay."
        sudo cp "${MODULE_KO}-overlay.dtbo" ${OVERLAYS_DIR}
    else
        echo "⚠️  ${MODULE_KO}-overlay.dtbo not found; build step may have failed."
    fi

    sudo touch "$uenv"

    # enable uboot overlays (uncomment or add)
    if sudo grep -Eq '^[[:space:]]*#?enable_uboot_overlays=' "$uenv"; then
        sudo sed -i 's/^[[:space:]]*#\{0,1\}enable_uboot_overlays=.*/enable_uboot_overlays=1/' "$uenv"
    else
        echo "enable_uboot_overlays=1" | sudo tee -a "$uenv" >/dev/null
    fi

    # already active?
    if sudo grep -Eq "^[[:space:]]*uboot_overlay_addr[0-9]+=.*${dtbo}" "$uenv"; then
        echo "✅ uEnv already contains ${dtbo}"
        return 0
    fi

    # exact commented entry for this dtbo -> uncomment it
    if sudo grep -Eq "^[[:space:]]*#.*uboot_overlay_addr[0-9]+=.*${dtbo}" "$uenv"; then
        sudo sed -i "s|^[[:space:]]*#\s*\(uboot_overlay_addr[0-9]+=.*${dtbo}\)|\1|" "$uenv"
        echo "✅ Uncommented existing overlay entry for ${dtbo}"
        return 0
    fi

    # prefer addr0..3 group, then addr4..7 group — try to replace the first commented slot in each group
    local ranges=("0 1 2 3" "4 5 6 7")
    for r in "${ranges[@]}"; do
        for n in $r; do
            # if there's a commented placeholder for this slot, replace it
            if sudo grep -Eq "^[[:space:]]*#.*uboot_overlay_addr${n}=.*" "$uenv"; then
                sudo sed -i "s|^[[:space:]]*#.*uboot_overlay_addr${n}=.*|uboot_overlay_addr${n}=${dtbo}|" "$uenv"
                echo "✅ Set uboot_overlay_addr${n} -> ${dtbo}"
                return 0
            fi
        done
    done

    # fallback: append into first unused slot 0..7
    local used
    used=$(sudo sed -n 's/^[[:space:]]*uboot_overlay_addr\([0-9]\+\)=.*/\1/p' "$uenv" | sort -n | uniq || true)
    for n in 0 1 2 3 4 5 6 7; do
        if ! printf '%s\n' "$used" | grep -qx "$n"; then
            echo "uboot_overlay_addr${n}=${dtbo}" | sudo tee -a "$uenv" >/dev/null
            echo "✅ Added uboot_overlay_addr${n} -> ${dtbo}"
            return 0
        fi
    done

    echo "⚠️  No free uboot_overlay_addr slots (0..7) in $uenv"
    return 1
}

mount_module() {
    echo "🔧 Building module..."
    make modules

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

    echo "✅ Cleanup complete."
}

case "${1:-}" in
    mount) mount_module ;;
    clean) clean_module ;;
    set_overlay) set_overlay ;;
    *) echo "Usage: $0 {mount|clean|set_overlay}"; exit 1 ;;
esac
