# Mounting StuffIt archives on the Mac 68k FujiNet

Status 2026-09-18: **working**. Now_Software.sit (13 MB, StuffIt 5 /
Arsenic) mounted in slot 1 from the web UI on a 512Ke: the ESP32 unstuffs
"Install 2.img" (~85 s over TNFS), decodes the Disk Copy 6 image to a
1.44 MB HFS volume, mounts it as an HD20, shows `archive -> image` in the
slot line and serves the raw image at `/sitdownload?deviceslot=N`
(verified byte-identical to ndif2raw). Booted from MacSpeak in slot 5,
the volume appears on the desktop.

Practical notes:
* Mount the archive **before** the floppy: Arsenic needs 2.6 MB of PSRAM
  scratch, NDIF decoding briefly holds the compressed fork and the raw
  image together, and an encoded 400K/800K floppy takes 0.6/1.2 MB.
* MacSpeak.dsk only boots when mounted read/write (its System writes to
  the boot disk); a locked mount gives a happy Mac, an eject and the "?".
* `./build.sh -f` (needed for web UI changes) rebuilds the filesystem
  from `data/webui/template/fnconfig.tmpl.ini` and wipes the device
  config (WiFi, hosts, slots). Save the config first.

Goal: the user picks a `.sit` in the web UI, the ESP32 unstuffs the disk
image inside it, works out whether it is a floppy image or an HD20
volume/drive image, mounts it in the chosen slot, and offers the unstuffed
image for download from the UI.

## What the archives look like

Surveyed with `lsar` (The Unarchiver):

| Archive kind | Magic | Methods seen | Where |
|---|---|---|---|
| StuffIt 5 (1997+) | `StuffIt (c)1997-` | 15 Arsenic (BWT + arithmetic coding), 0 none | most repository downloads |
| Classic SIT! (1.5 to 4.x) | `SIT!` at 0, `rLau` at 10 | 13 LZ+Huffman, 0, plus 1 RLE / 2 LZW / 3 Huffman in old files | older files, `.sea` self-extractors carry the same data after the code |

Entries have a data fork and a resource fork, each with its own method,
compressed/uncompressed length and CRC-16. Disk images are the data fork.
Inner images are DiskCopy 4.2 (`.image`, 84-byte header, magic 0x0100),
raw 400K/800K sector dumps, or HFS volumes / drive images for the HD20.

Reference implementation: The Unarchiver (`ref_XADMaster/`, LGPL):
`XADStuffItParser.m`, `XADStuffIt5Parser.m`, `XADStuffItOldHandles.m`
(RLE90, LZW, Huffman), `XADStuffIt13Handle.m`, `XADStuffItArsenicHandle.m`
with `BWT.c`, `LZW.c`, `XADPrefixCode.m`. `unar`/`lsar` are the oracle.

## Phase 0: `lib/stuffit/`, pure C, host tested

* `stuffit.h/.c`: open an archive from a `FILE*` (both formats), iterate
  entries: path, type/creator, data-fork method, compressed and
  uncompressed sizes, CRC, offset; skip folders, resource forks,
  encrypted entries (report, do not fail the whole archive).
* Decompressors, streaming, `int (*sink)(const uint8_t*, size_t, void*)`:
  method 0, 1 (RLE90), 2 (LZW 14-bit), 3 (Huffman), 13 (LZ+Huffman),
  15 (Arsenic). Input is read from the `FILE*` through a small buffer;
  no method may need the whole compressed fork in memory. Arsenic needs
  `blocksize + 4*blocksize` bytes of scratch (block size is in the
  stream, 2^9..2^24); allocate through a caller-supplied allocator so the
  ESP32 can use PSRAM, and report the block size so oversize archives
  fail cleanly.
* CRC-16 check of every extracted fork.
* Host tool `tests/unsit` (list, extract one entry to a file) and a test
  target that extracts every entry of the samples in
  `~/code/FujiNet_macOS_2026/sit_samples/` and compares with `unar`.
* Coding rules: C99, no globals, no recursion deeper than a few levels,
  no `alloca`, bounded memory, `-Wall -Wextra` clean, every allocation
  through `sit_alloc/sit_free` callbacks. Same license header style as
  the rest of `lib/`.

## Phase 0b: BinHex and NDIF (same library)

* `.hqx` (BinHex 4.0) wraps most classic `.sit` downloads: 6-bit alphabet,
  RLE90, three CRCs. Decoded into a buffer and handed to `sit_open()`.
* Disk Copy 6 **NDIF** `.img` files are common inside the archives
  (e.g. every image in Now_Software.sit): chunks in the data fork, block
  map in a `bcem` resource in the resource fork, chunks raw, zero, ADC or
  KenCode compressed. Reference and oracle: `ref_ndif2raw/` (BSD, C, has
  a resource-fork parser). Output is a raw HFS volume, usually 1.44 MB,
  which this hardware can only serve as an HD20 volume, never as a GCR
  floppy. So the classification is: 400K/800K raw or DiskCopy 4.2 →
  floppy path; anything else that is HFS → HD20 path.

## Phase 1: ESP32 integration (Mac build)

* `MediaType::discover_mediatype`: `.sit` (and `.sea`) → `MEDIATYPE_SIT`.
* `macFloppy::mount()` for that type: open the archive, choose the disk
  image entry (extension `.image/.img/.dsk/.dc42/.hda/.dmg`, else the
  largest data fork), extract it into a PSRAM buffer (cap 3 MB; a bigger
  image needs a writable host directory, see below), wrap it with
  `fmemopen()` and hand it to the existing mount path with the inner
  filename, so floppy vs HD20 classification is the code that exists
  today (slot 5 = floppy, slots 1-4 = HD20, DC42/size/HFS detection).
* Keep the buffer for the device's lifetime; free it on unmount.
* If the archive's host directory is writable (TNFS with write access)
  and the image is bigger than the PSRAM cap, or the user asks for it,
  write the extracted image next to the archive and mount that file
  instead. This is also how an HD20 volume from a `.sit` becomes
  persistent.
* Web UI: the slot line shows `archive.sit → inner.image`; a Download
  link hits `/sitdownload?deviceslot=N`, which streams the buffer (or the
  extracted host file) with the inner file name. Extraction happens
  inside the mount request, like the encoder does now; log progress on
  the console.
* Memory budget: 800K image (0.8 MB) + encoded tracks (1.2 MB) + Arsenic
  scratch (5 × block) must fit the ~4 MB of PSRAM that is free with
  nothing mounted.

## Phase 2

* Multi-image archives: list entries in the UI and let the user pick.
* `.hqx` (BinHex) and MacBinary wrappers around `.sit`.
* Compact Pro `.cpt`.
* Write-back: with an image mounted from PSRAM, offer "save to host".

## Out of scope

StuffIt X (`.sitx`), encrypted archives, StuffIt 5 methods other than
0/13/15. Resource fork *extraction* (raw bytes of either fork, via
`sit_extract`'s fork selector) is in scope as of Phase 0b, needed for
NDIF's `bcem` block map; interpreting resource-fork *contents* beyond
that (a general resource-fork/Finder-info parser) stays out of scope.
