# KNAON label printer CUPS filter (reconstructed)

This repository contains a compilable C reconstruction of the KNAON Linux
printer driver shipped in:

- `Knaon-Linux-Driver-Latest_Ver1.1.1.run`
- `Knaon_Linux_Driver/knaon/Filter/rastertolabel`
- `Knaon_Linux_Driver/KnaonLabel.ppd`

The vendor binary is an x86-64 ELF that was **not stripped and included full
DWARF debug information**, which made a high-fidelity reconstruction possible.

## What was recovered

The vendor filter is a modified copy of the CUPS 2.2.7 sample filter
`filter/rastertolabel.c`. The vendor added:

- a CUPS device-URI check that only accepts printer model names such as
  `Y468`, `Y486`, `A318`, `Y810`, `Y528`, `Y528BT`, `Y812`, `Y813`, `RH40`,
  `RH40BT`, and `Y480`;
- a KNAON-specific output path for `*cupsModelNumber: 20`, using the
  printer's `SIZE`, `REFERENCE`, `GAP`/`BLINE`, `DENSITY`, `SPEED`,
  `SETC`, `CLS`, `BITMAP`, and `PRINT` commands;
- image rotation (`Rotate`), brightness (`Brightness`), contrast
  (`Contrast`), thresholding (`Threshold`), and two error-diffusion
  dithering modes (`PrintEffect` = `Effect1`/`Effect2`);
- several unused image helpers (`sobel`, `erosion`, `dilation`) that are
  present in the binary but not called.

The reconstruction was derived from:

- the DWARF debug information (function names, local variable names/types,
  line numbers, global variables, and source path);
- Ghidra decompilation and `objdump` disassembly for the exact logic and
  floating-point constants;
- the upstream CUPS 2.2.7 `rastertolabel.c` for the unmodified printer
  branches;
- differential testing against the vendor binary.

## Layout

```
Makefile                  Build/install rules
ppd/KnaonLabel.ppd        PPD file (copied from the vendor package)
src/rastertolabel.c       Reconstructed filter source
tests/raster_gen.c        CUPS raster test-file generator
tests/diff_test.sh        Differential test against the vendor binary
```

## Build

Requirements:

- a C compiler, `make`
- CUPS development files (`cups-config`, `libcups`)
- `python3` for the differential test only

On Debian/Ubuntu:

```sh
sudo apt install build-essential libcups2-dev
make
```

The result is `build/rastertolabel`.

## Install

The PPD refers to the filter as `knaon/Filter/rastertolabel`, so CUPS expects
it below its filter directory:

```sh
sudo make install
```

This installs:

- `/usr/lib/cups/filter/knaon/Filter/rastertolabel` (path may differ by distro)
- `/usr/share/ppd/KnaonLabel.ppd`

Then add a printer using the PPD, for example with the CUPS web interface
(`http://localhost:631`) or `lpadmin`.

## Filter invocation

CUPS invokes the filter as:

```
rastertolabel job-id user title copies options [raster-file]
```

`PPD` must point to `KnaonLabel.ppd`. Example manual run:

```sh
PPD=$PWD/ppd/KnaonLabel.ppd \
  build/rastertolabel 1 user title 1 "" page.rast > printer-data.bin
```

Options are passed in the fifth argument, e.g.:

```sh
PPD=$PWD/ppd/KnaonLabel.ppd build/rastertolabel 1 user title 1 \
  "PrintEffect=Effect1 Brightness=160 Contrast=180 Threshold=140 Rotate=0" \
  page.rast > printer-data.bin
```

## Verification

`tests/diff_test.sh` builds a test variant of the reconstruction and compares
it byte-for-byte with a copy of the vendor binary in which only the
device-URI/printer-model check has been bypassed (so it can run without a
configured CUPS queue).

```sh
make build/rastertolabel build/rastertolabel-test build/raster_gen
tests/diff_test.sh
```

The test matrix covers:

- multiple raster sizes, including odd widths;
- gradient, checkerboard, noise, stripe, text-like, and solid images;
- multi-page jobs;
- all four rotation modes;
- both dithering effects, alone and combined with rotation;
- brightness, contrast, and threshold sweeps across their full PPD ranges;
- media tracking, gap/mark settings, horizontal/vertical offsets, darkness,
  print speed, and dotted-line settings;
- combined option sets.

All 39 cases match the vendor binary exactly on stdout and stderr.

## Reconstruction notes and intentional deviations

The source intentionally follows the vendor binary's behavior, including a
few quirks that are harmless but visible when reading the code:

- `ProcessImage()` only emits image data when `ModelNumber == 0x14`
  (`*cupsModelNumber: 20`). Other printer branches remain for fidelity but
  produce no bitmap data.
- The KNAON `StartPage()` path builds four diagnostic strings with
  `sprintf()` but never outputs them. They are retained because they are
  present in the vendor binary.
- `ProcessImage()` assumes the raster stride equals `cupsWidth`
  (8-bit grayscale, one byte per pixel).

Deliberate, behavior-preserving changes:

- `g_GammaLUT` is declared as 256 bytes. The vendor binary declares
  `UNIT8 g_GammaLUT[255]` but writes index 255, which lands in adjacent
  padding; using 256 elements removes undefined behavior without changing
  output.
- The dither matrices are static arrays instead of per-page heap
  allocations. This removes a small leak while producing identical output.
- `KNANO_SKIP_DEVICE_CHECK` is an optional compile-time test hook used only
  by `build/rastertolabel-test`. The normal build performs the same
  device-URI check as the vendor binary.

## Notes on the embedded installer

`Knaon-Linux-Driver-Latest_Ver1.1.1.run` is a shell script followed by a
gzip-compressed tar archive. Its archive contains exactly the same PPD and
filter binary found in `Knaon_Linux_Driver/`; it does not contain additional
source code, firmware, or tools.
