#!/usr/bin/env bash
set -euo pipefail

usb_identity() {
    local vendor product
    [[ -r "$1/idVendor" && -r "$1/idProduct" ]] || return 1
    read -r vendor < "$1/idVendor" || return 1
    read -r product < "$1/idProduct" || return 1
    printf '%s:%s\n' "$vendor" "$product"
}

find_keyboard() {
    local candidate
    port=''
    for candidate in /sys/bus/usb/devices/*; do
        [[ "$(usb_identity "$candidate" || true)" == '8d1d:343a' ]] || continue
        if [[ -n "$port" ]]; then
            echo 'Connect only the Corne half you want to flash, then retry.' >&2
            return 1
        fi
        port=$candidate
    done
    if [[ -z "$port" ]]; then
        echo 'Connect one Corne half in normal keyboard mode before starting.' >&2
        return 1
    fi
    read -r serial < "$port/serial"
    printf 'Selected Corne %s on USB port %s.\n' "$serial" "${port##*/}"
}

find_raw_hid() {
    local port_path hid hid_path candidate
    port_path=$(readlink -f "$port") || return 1
    raw_hid_device=''
    for hid in /sys/class/hidraw/*; do
        hid_path=$(readlink -f "$hid/device") || continue
        [[ "$hid_path" == "$port_path/"* ]] || continue
        # QMK Raw HID: vendor usage page 0xFF60, usage 0x61, application collection.
        cmp -s -n 7 "$hid/device/report_descriptor" <(printf '\x06\x60\xff\x09\x61\xa1\x01') || continue
        candidate=/dev/${hid##*/}
        [[ -c "$candidate" ]] || continue
        if [[ -n "$raw_hid_device" ]]; then
            echo 'Multiple Raw HID interfaces on the keyboard port; refusing to choose.' >&2
            return 1
        fi
        raw_hid_device=$candidate
    done
    if [[ -z "$raw_hid_device" ]]; then
        echo 'No Corne Raw HID interface. Run ./deploy.sh --manual once to install trigger support.' >&2
        return 1
    fi
}

bootloader_report() {
    # Linux hidraw needs report ID zero followed by the 32-byte QMK payload.
    printf '\0%-32s' 'CORNE_BOOTLOADER_V1' | tr ' ' '\0'
}

request_bootloader() {
    local current_serial
    if [[ "$(usb_identity "$port" || true)" != '8d1d:343a' ]]; then
        echo 'The selected Corne is no longer connected in normal mode.' >&2
        return 1
    fi
    read -r current_serial < "$port/serial" || return 1
    if [[ "$current_serial" != "$serial" ]]; then
        echo 'The controller on the selected port changed; refusing to trigger it.' >&2
        return 1
    fi
    find_raw_hid || return 1
    printf 'Requesting BOOTSEL through %s.\n' "$raw_hid_device"
    bootloader_report | timeout 5 dd of="$raw_hid_device" bs=33 count=1 iflag=fullblock oflag=nonblock conv=nocreat,notrunc status=none
}

find_bootloader_partition() {
    local port_path block block_path candidate
    device=''
    [[ "$(usb_identity "$port" || true)" == '2e8a:0003' ]] || return 0
    port_path=$(readlink -f "$port") || return 1
    for block in /sys/class/block/*; do
        [[ -f "$block/partition" ]] || continue
        block_path=$(readlink -f "$block") || continue
        [[ "$block_path" == "$port_path/"* ]] || continue
        candidate=/dev/${block##*/}
        [[ -b "$candidate" ]] || continue
        if [[ -n "$device" ]]; then
            echo 'Multiple partitions on the keyboard port; refusing to choose.' >&2
            return 1
        fi
        device=$candidate
    done
}

cleanup_mount() {
    if [[ -n "$owned_mount" ]]; then
        umount "$owned_mount" || true
        rmdir "$owned_mount" || true
    fi
}

flash_keyboard() {
    local firmware=$1 mode=$2 deadline mount_dir info current_serial
    [[ -s "$firmware" ]] || { echo "Missing firmware: $firmware" >&2; return 1; }
    owned_mount=''
    trap cleanup_mount EXIT
    if [[ "$mode" == 'manual' ]]; then
        printf 'READY: Double-tap reset on the USB-connected half. Waiting up to 10 minutes on port %s.\n' "${port##*/}"
        deadline=$((SECONDS + 600))
    else
        request_bootloader || return 1
        echo 'Bootloader request sent. Waiting up to 30 seconds for the UF2 drive.'
        deadline=$((SECONDS + 30))
    fi
    device=''
    while (( SECONDS < deadline )); do
        find_bootloader_partition || return 1
        [[ -z "$device" ]] || break
        sleep 0.25
    done
    if [[ -z "$device" ]]; then
        echo 'Timed out waiting for the keyboard UF2 drive.' >&2
        return 1
    fi
    printf 'Detected keyboard bootloader partition: %s\n' "$device"
    mount_dir=$(findmnt -rn -S "$device" -o TARGET || true)
    if [[ -z "$mount_dir" ]]; then
        mount_dir=$(mktemp -d /tmp/corne-uf2.XXXXXX)
        owned_mount=$mount_dir
        mount -t vfat -o nodev,nosuid,noexec "$device" "$mount_dir"
    fi
    info=$(< "$mount_dir/INFO_UF2.TXT")
    if [[ "$info" != *'Board-ID: RPI-RP2'* ]]; then
        echo 'Not an RPI-RP2 UF2 bootloader; refusing to write.' >&2
        return 1
    fi
    echo 'Copying firmware to the UF2 drive.'
    dd if="$firmware" of="$mount_dir/NEW.UF2" conv=fsync status=none
    echo 'UF2 copy and flush complete. Waiting for the keyboard to reconnect.'
    deadline=$((SECONDS + 30))
    while (( SECONDS < deadline )); do
        current_serial=''
        if [[ "$(usb_identity "$port" || true)" == '8d1d:343a' && -r "$port/serial" ]]; then
            read -r current_serial < "$port/serial" || true
            if [[ "$current_serial" == "$serial" ]]; then
                echo 'FLASH_COMPLETE: UF2 copied and the expected Corne reconnected. No independent flash read-back performed.'
                return 0
            fi
        fi
        sleep 0.25
    done
    echo 'UF2 copy completed, but the expected keyboard did not reconnect within 30 seconds.' >&2
    return 1
}

main() {
    local script script_dir firmware mode
    script=$(readlink -f "${BASH_SOURCE[0]}")
    script_dir=${script%/*}
    firmware=$script_dir/splitkb_aurora_corne_rev1_miryoku_553.uf2
    if [[ $# == 0 || ( $# == 1 && "$1" == '--manual' ) ]]; then
        mode=auto
        [[ $# == 0 ]] || mode=manual
        find_keyboard
        cd "$script_dir"
        qmk compile -kb splitkb/aurora/corne/rev1 -km miryoku_553
        # Authenticate while the keyboard still works; keep privilege for the bounded flash step.
        exec sudo "$BASH" "$script" --flash "$port" "$serial" "$mode"
    elif [[ $# == 4 && "$1" == '--flash' && $EUID == 0 && ( "$4" == 'auto' || "$4" == 'manual' ) ]]; then
        port=$2
        serial=$3
        flash_keyboard "$firmware" "$4"
    else
        echo 'Usage: ./deploy.sh [--manual] (start with one Corne half connected in normal mode)' >&2
        return 1
    fi
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi
