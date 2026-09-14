# ROM assets — provenance

ROMs are not in this repository (see `.gitignore`) — they're Sharp
Corporation's copyrighted firmware. Run
[`tools/fetch_roms.sh`](../tools/fetch_roms.sh) to (re)download the 12
with a known public source into this directory before building; it
verifies each against the md5 in the table below.

| File | Source | Model / role | Confidence |
|---|---|---|---|
| `PC-1500_A04.ROM` | [Jeff-Birt/Sharp_PC-1500_ROM_Disassembly](https://github.com/Jeff-Birt/Sharp_PC-1500_ROM_Disassembly) (`Original_ROMs/`) | Runs on **both** PC-1500 and PC-1500A — originally developed for the PC-1500A's release, later also used in later-production PC-1500 (non-A) units. Sole/default ROM for the PC-1500A-only build; also one of three options in the PC-1500 ROM selector. | Confirmed |
| `PC-1500_A01.ROM` | [Jeff-Birt/Sharp_PC-1500_ROM_Disassembly](https://github.com/Jeff-Birt/Sharp_PC-1500_ROM_Disassembly) (`Original_ROMs/`) | PC-1500 (non-A) **only** — must not be offered for PC-1500A. | Confirmed |
| `PC-1500_A03.ROM` | [Jeff-Birt/Sharp_PC-1500_ROM_Disassembly](https://github.com/Jeff-Birt/Sharp_PC-1500_ROM_Disassembly) (`Original_ROMs/`) | PC-1500 (non-A) **only**, same as A01. | Confirmed |
| `CE-150.ROM` | **None yet.** Not fetched by `fetch_roms.sh`. The Jeff-Birt repo (previously cited here) only has `lib/CE-150.lib`, a symbol table, not the ROM binary itself; no other public source is known -- needs a fresh hardware dump. If you have your own copy, keep it here locally (gitignored); without it, CE-150 support is unavailable in any build assembled via `fetch_roms.sh` (currently: CI builds -- local builds work if you have your own copy). | CE-150 printer/plotter/cassette-interface firmware, 8192 bytes, md5 `eb9aa5156c6849890b137799efc50a4b`. Runs on the host LH5801 from guest 0xA000–0xBFFF (its pc1500 and pc1500A copies are byte-identical). Also bundled as `Calc-U-1600/Resources/CE-150.bin`. | **Unverified against real hardware.** |
| `PC1600-LH5803-C000-FFFF.bin` | [tinue/PC-1600-ROM](https://github.com/tinue/PC-1600-ROM) (`dumps/`) | PC-1600's LH5803 co-processor ROM (its own C000–FFFF) | Confirmed, dumped from real hardware |
| `PC1600-P0-B0.bin` | [tinue/PC-1600-ROM](https://github.com/tinue/PC-1600-ROM) (`dumps/`) | Bank 0 lower half, 0x0000, system ROM (CS001, never switched out) | Confirmed, dumped from real hardware |
| `PC1600-P1-B0.bin` | [tinue/PC-1600-ROM](https://github.com/tinue/PC-1600-ROM) (`dumps/`) | Bank 0 upper half, 0x4000 | Confirmed, dumped from real hardware |
| `PC1600-P1-B3.bin` | [tinue/PC-1600-ROM](https://github.com/tinue/PC-1600-ROM) (`dumps/`) | Bank 3, 0x4000 (CS24 normal half) | Confirmed, dumped from real hardware |
| `PC1600-P1-B3B.bin` | [tinue/PC-1600-ROM](https://github.com/tinue/PC-1600-ROM) (`dumps/`) | Bank 3b, 0x4000, hidden BASIC ROM (Port 3DH bit b2) | Confirmed, dumped from real hardware |
| `PC1600-P2-B6.bin` | [tinue/PC-1600-ROM](https://github.com/tinue/PC-1600-ROM) (`dumps/`) | Bank 6, 0x8000 (display/timer/serial/char tables, CS123) | Confirmed, dumped from real hardware |
| `PC1600-P1-B4-CE1600P.bin` | [tinue/PC-1600-ROM](https://github.com/tinue/PC-1600-ROM) (`dumps/`) | CE-1600P plotter ROM, lower 16KB half (card-local 0x0000-0x3FFF), banked onto the PC-1600's Page B bank 4 via the PV pin | Confirmed, dumped from real hardware |
| `PC1600-P1-B5-CE1600P-OR-F.bin` | [tinue/PC-1600-ROM](https://github.com/tinue/PC-1600-ROM) (`dumps/`) | CE-1600P plotter ROM, upper 16KB half (card-local 0x4000-0x7FFF), banked onto Page B bank 5 via the PV pin | Confirmed, dumped from real hardware |
| `CE-158.ROM` | [Jeff-Birt/Sharp_CE-158](https://github.com/Jeff-Birt/Sharp_CE-158) (`CE-158_ROM_ORIG.bin`) | CE-158 cassette-interface firmware, 16384 bytes, md5 `aa952878fb29da4844791d95185649ca`. Not yet wired into any card implementation here -- fetched in advance of that work. | Confirmed |

All 8 PC-1600 images are our own dumps, pulled straight off real Sharp
PC-1600 hardware with a purpose-built ROM-dumper cartridge tool (see
[tinue/PC-1600-ROM](https://github.com/tinue/PC-1600-ROM) for the dumper
source and full provenance writeup), and confirmed byte-identical to
[PockEmul](https://pockemul.com)'s copies.
