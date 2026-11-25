#!/usr/bin/env bash
# Timestamped watch of the host side of the X310 link (no sudo needed).
#
# Prints one line whenever a drop/error counter moves, plus a heartbeat every
# 30 s with the packet rate. Run it next to rf_monitor_gui and compare the
# timestamps with the app's "UsrpCapture::capture (...)" / "Scanner: ...
# reconnecting" lines and UHD's D runs:
#   - counters move at the same moment  -> the host NIC dropped the packets
#   - counters stay flat during a burst -> the loss is on the wire or in the X310
#
# Usage: tools/x310_link_watch.sh [iface] | tee x310_link_$(date +%H%M).log
set -u
IFACE=${1:-enp0s31f6}
KEYS="rx_missed_errors rx_no_buffer_count rx_crc_errors rx_length_errors rx_long_length_errors rx_errors"

read_nic() { ethtool -S "$IFACE" 2>/dev/null | awk -v keys="$KEYS rx_packets" '
    BEGIN { n = split(keys, k, " "); for (i = 1; i <= n; i++) want[k[i]] = 1 }
    { gsub(":", "", $1); if ($1 in want) v[$1] = $2 }
    END { for (i = 1; i <= n; i++) printf "%s ", (k[i] in v ? v[k[i]] : 0) }'; }
read_udp() { awk '/^Udp:/ { if (++n == 2) print $3, $4, $6 }' /proc/net/snmp; }  # NoPorts InErrors RcvbufErrors
read_carrier() { cat "/sys/class/net/$IFACE/carrier_changes" 2>/dev/null || echo 0; }

names=($KEYS rx_packets udp_noports udp_inerrors udp_rcvbuferrors carrier_changes)
prev=($(read_nic) $(read_udp) $(read_carrier))
echo "$(date +%T) watching $IFACE; ring: $(ethtool -g "$IFACE" 2>/dev/null | awk '/^RX:/ {print $2}' | tail -1)"
beat=0
while sleep 1; do
    cur=($(read_nic) $(read_udp) $(read_carrier))
    line=""
    for i in "${!names[@]}"; do
        [ "${names[$i]}" = rx_packets ] && continue
        d=$(( ${cur[$i]:-0} - ${prev[$i]:-0} ))
        [ "$d" -ne 0 ] && line+=" ${names[$i]}+$d"
    done
    pps_idx=$(( $(echo $KEYS | wc -w) ))
    pps=$(( ${cur[$pps_idx]:-0} - ${prev[$pps_idx]:-0} ))
    if [ -n "$line" ]; then echo "$(date +%T.%3N) DROP$line (rx ${pps} pkt/s)"; fi
    beat=$(( beat + 1 ))
    if [ $beat -ge 30 ]; then echo "$(date +%T) ok rx ${pps} pkt/s"; beat=0; fi
    prev=("${cur[@]}")
done
