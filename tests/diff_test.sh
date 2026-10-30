#!/bin/sh
# Differential test: compare the reconstructed filter against the vendor
# binary for a matrix of raster sizes, image patterns and PPD options.
#
# Usage: tests/diff_test.sh [path-to-vendor-rastertolabel]
#
# The vendor binary is copied to build/orig-rastertolabel and its call to
# getDeviceUriWithPrinterName() is replaced with a jump, so it can run
# without a configured CUPS printer queue.

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VENDOR=${1:-$ROOT/Knaon_Linux_Driver/knaon/Filter/rastertolabel}
WORK=$ROOT/build/diff
ORIG=$ROOT/build/orig-rastertolabel

mkdir -p "$WORK"

make -s -C "$ROOT" build/rastertolabel-test build/raster_gen

if [ ! -x "$ORIG" ]; then
  cp "$VENDOR" "$ORIG"
  python3 - "$ORIG" <<'EOF'
import sys
p = sys.argv[1]
b = bytearray(open(p, 'rb').read())
# At file offset 0x1695 the vendor binary calls getDeviceUriWithPrinterName.
# Replace that call with a jump to 0x179a (the success path).
b[0x1695:0x169a] = bytes([0xE9, 0x00, 0x01, 0x00, 0x00])
open(p, 'wb').write(b)
EOF
  chmod +x "$ORIG"
fi

export PPD=$ROOT/ppd/KnaonLabel.ppd

fail=0
count=0

run_case() {
  name=$1
  width=$2
  height=$3
  pattern=$4
  pages=$5
  options=$6

  count=$((count + 1))
  rast=$WORK/$name.rast

  "$ROOT/build/raster_gen" "$rast" "$width" "$height" "$pattern" "$pages"

  "$ORIG" 1 user title 1 "$options" "$rast" >"$WORK/$name.orig.out" 2>"$WORK/$name.orig.err" || true
  "$ROOT/build/rastertolabel-test" 1 user title 1 "$options" "$rast" \
      >"$WORK/$name.ours.out" 2>"$WORK/$name.ours.err" || true

  if ! cmp -s "$WORK/$name.orig.out" "$WORK/$name.ours.out"; then
    echo "FAIL stdout: $name" >&2
    fail=1
  fi

  if ! cmp -s "$WORK/$name.orig.err" "$WORK/$name.ours.err"; then
    echo "FAIL stderr: $name" >&2
    fail=1
  fi
}

run_case basic64      64  48  gradient 1 ""
run_case basic283     283 425 gradient 1 ""
run_case odd75        75  101 checker  1 ""
run_case noise128     128 200 noise    1 ""
run_case stripes300   300 364 stripes  1 ""
run_case text200      200 160 text     1 ""
run_case solid100     100 100 solid    1 ""
run_case multipage    96  128 gradient 3 ""

run_case rot1         283 425 gradient 1 "Rotate=1"
run_case rot2         283 425 gradient 1 "Rotate=2"
run_case rot3         283 425 gradient 1 "Rotate=3"

run_case effect1      283 425 gradient 1 "PrintEffect=Effect1"
run_case effect2      283 425 gradient 1 "PrintEffect=Effect2"
run_case effect1noise 128 200 noise    1 "PrintEffect=Effect1"
run_case effect2text  200 160 text     1 "PrintEffect=Effect2"
run_case effect1rot1  96  128 checker  1 "PrintEffect=Effect1 Rotate=1"
run_case effect2rot2  96  128 checker  1 "PrintEffect=Effect2 Rotate=2"

run_case bright0      128 96  gradient 1 "Brightness=0"
run_case bright64     128 96  gradient 1 "Brightness=64"
run_case bright200    128 96  gradient 1 "Brightness=200"
run_case bright255    128 96  gradient 1 "Brightness=255"
run_case contrast0    128 96  gradient 1 "Contrast=0"
run_case contrast64   128 96  gradient 1 "Contrast=64"
run_case contrast200  128 96  gradient 1 "Contrast=200"
run_case contrast255  128 96  gradient 1 "Contrast=255"
run_case thresh0      128 96  gradient 1 "Threshold=0"
run_case thresh64     128 96  gradient 1 "Threshold=64"
run_case thresh200    128 96  gradient 1 "Threshold=200"
run_case thresh255    128 96  gradient 1 "Threshold=255"

run_case media_cont   128 96  gradient 1 "zeMediaTracking=Continuous"
run_case media_bline  128 96  gradient 1 "zeMediaTracking=BLine GapOrMarkHeight=7 GapOrMarkOffset=3"
run_case offsets      128 96  gradient 1 "AdjustHoriaontal=-5 AdjustVertical=6"
run_case darkness0    128 96  gradient 1 "Darkness=0"
run_case darkness15   128 96  gradient 1 "Darkness=15"
run_case speed1       128 96  gradient 1 "zePrintRate=1"
run_case speed8       128 96  gradient 1 "zePrintRate=8"
run_case autodotted   128 96  gradient 1 "AutoDotted=1"

run_case combo        283 425 gradient 1 \
  "Rotate=2 PrintEffect=Effect1 Brightness=160 Contrast=180 Threshold=140 Darkness=10 zePrintRate=6 AdjustHoriaontal=2 AdjustVertical=-3"
run_case combo2       200 160 text     2 \
  "Rotate=3 PrintEffect=Effect2 Brightness=90 Contrast=70 Threshold=220 zeMediaTracking=Continuous AutoDotted=1"

if [ "$fail" -ne 0 ]; then
  echo "FAILED ($count cases)" >&2
  exit 1
fi

echo "All $count differential cases matched."
