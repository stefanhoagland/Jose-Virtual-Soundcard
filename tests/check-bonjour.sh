#!/usr/bin/env bash
# macOS only: start a Dante device and check that the Mac's own Bonjour
# (which Dante Controller uses) can find its name, address and ports.
set -u
POC=${1:-build/jvs-poc}
IF=$(route -n get default | awk '/interface:/{print $2}')
IP=$(ipconfig getifaddr "$IF")
NAME=Jose-Bonjour-Test
echo "Interface $IF ($IP)"
"$POC" ptp-master -i "$IF" --ptp 1 > master.log 2>&1 &
MASTER=$!
"$POC" device -i "$IF" --rx 2 --tx 2 --name "$NAME" > device.log 2>&1 &
DEV=$!
trap 'kill $DEV $MASTER 2>/dev/null' EXIT
sleep 12

probe() {  # probe <seconds> <dns-sd args...>
  local t=$1; shift
  dns-sd "$@" > probe.out 2>&1 &
  local p=$!
  sleep "$t"; kill $p 2>/dev/null
  cat probe.out
}
ok=1
echo "== ARC service"
probe 4 -L "$NAME" _netaudio-arc._udp local. | tee arc.out
grep -qi "$NAME.local.:4440" arc.out || ok=0
echo "== CMC service"
probe 4 -L "$NAME" _netaudio-cmc._udp local. | tee cmc.out
grep -qi "$NAME.local.:8800" cmc.out || ok=0
echo "== Address"
probe 4 -G v4 "$NAME.local" | tee a.out
grep -qi "$IP" a.out || ok=0
echo "== Channels"
probe 4 -B _netaudio-chan._udp local. | tee chan.out
grep -qi "TX 1@$NAME" chan.out || ok=0
if [ $ok = 1 ]; then echo "Bonjour check passed"; exit 0; fi
echo "Bonjour check FAILED"; echo "--- device log"; tail -40 device.log; exit 1
