#!/bin/bash
# Shared helpers; only touch daemons owned by this config/interface.
[[ "$IFACE" =~ ^[a-zA-Z0-9_.:-]+$ ]] || exit 1

daemon_alive() {
    local pid arg owned=0
    read -r pid 2>/dev/null < "$CONF/$1.pid" || return 1
    [[ "$pid" =~ ^[1-9][0-9]*$ ]] || return 1
    [ "$(cat "/proc/$pid/comm" 2>/dev/null)" = "$1" ] || return 1
    while IFS= read -r -d '' arg; do
        [[ "$arg" = "$CONF/$1.conf" || "$arg" = "--conf-file=$CONF/$1.conf" ]] && owned=1
    done 2>/dev/null < "/proc/$pid/cmdline"
    [ "$owned" = 1 ] && kill -0 "$pid" 2>/dev/null
}

process_owned() {
    local name=$1 pid=$2 arg owned=0
    [[ "$pid" =~ ^[1-9][0-9]*$ && "$pid" -gt 1 ]] || return 1
    [ "$(cat "/proc/$pid/comm" 2>/dev/null)" = "$name" ] || return 1
    while IFS= read -r -d '' arg; do
        if [[ "$name" = hostapd || "$name" = dnsmasq ]]; then
            [[ "$arg" = "$CONF/$name.conf" || "$arg" = "--conf-file=$CONF/$name.conf" ]] && owned=1
        else
            [[ "$arg" = "$IFACE" || "$arg" = "-i$IFACE" || "$arg" = "dhcpcd: $IFACE "* ]] && owned=1
        fi
    done 2>/dev/null < "/proc/$pid/cmdline"
    [ "$owned" = 1 ]
}

stop_owned() {
    local name=$1 pid i
    for pid in $(pgrep -x "$name"); do
        process_owned "$name" "$pid" || continue
        kill -TERM "$pid" 2>/dev/null || true
        for i in {1..10}; do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
        # A driver ioctl can ignore TERM. Recheck process identity before KILL.
        if process_owned "$name" "$pid"; then
            kill -KILL "$pid" 2>/dev/null || true
        fi
    done
    rm -f "$CONF/$name.pid"
}

# Check only after acquiring wifi.lock: a queued stop/client from an older
# transition must not tear down the mode selected by a newer process.
mode_is_current() {
    local wanted
    [ -f "$CONF/desired-mode" ] || return 0
    read -r wanted < "$CONF/desired-mode" || return 1
    [ "$wanted" = "$1" ]
}

stop_clients() {
    stop_owned wpa_supplicant
    stop_owned dhcpcd
    stop_owned udhcpc
    stop_owned dhclient
}
