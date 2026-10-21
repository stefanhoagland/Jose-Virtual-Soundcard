#!/usr/bin/env bash
# End-to-end test on one machine: a fake PTP clock, a sender with two
# 8-channel flows, and a receiver that records one flow and checks the tones.
# Needs root on Linux (PTP uses ports 319/320).
set -euo pipefail
BIN=${1:-build/jvs-poc}
IFACE=${IFACE:-$(ip -4 route show default 2>/dev/null | awk '{print $5; exit}')}
IFACE=${IFACE:-eth0}
OUT=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$OUT"' EXIT

"$BIN" ptp-master -i "$IFACE" --seconds 40 >"$OUT/master.log" 2>&1 &
sleep 1
"$BIN" send -i "$IFACE" --flows 2 --channels 8 --seconds 35 >"$OUT/send.log" 2>&1 &
sleep 1
"$BIN" receive -i "$IFACE" --stream "JVS Test 2" --seconds 8 --latency-ms 20 --wav "$OUT/rx.wav" | tee "$OUT/rx.log"

python3 - "$OUT/rx.wav" <<'PY'
import struct, sys, math
d = open(sys.argv[1], 'rb').read()
ch = struct.unpack('<H', d[22:24])[0]
n = (len(d) - 44) // 4 // ch
x = struct.unpack('<%df' % (n * ch), d[44:44 + n * ch * 4])
assert ch == 8, ch
# Flow 2 carries channels 9-16: channel k plays 200 + 100*k Hz.
start, length = n // 2, 4800
bad = 0
for c in range(ch):
    want = 200 + 100 * (9 + c)
    seg = [x[(start + i) * ch + c] for i in range(length)]
    # Correlate with the expected tone (Goertzel-style power at that frequency).
    re = sum(s * math.cos(2 * math.pi * want * i / 48000) for i, s in enumerate(seg))
    im = sum(s * math.sin(2 * math.pi * want * i / 48000) for i, s in enumerate(seg))
    amp = 2 * math.hypot(re, im) / length
    print(f"channel {c+1}: {want} Hz amplitude {amp:.3f}")
    if not (0.10 < amp < 0.15):
        bad += 1
sys.exit(1 if bad else 0)
PY
echo "loopback test passed"
