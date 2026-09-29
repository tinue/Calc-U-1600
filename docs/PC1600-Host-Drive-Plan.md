# PC-1600 host-directory drive "S3:" / "Y:"

## Context
The user wants to share files between the Mac and the emulated PC-1600 without disk images: **File ▸ Mount Directory…** exposes a host folder as drive `S3:` (alias `Y:`) that BASIC uses like a RAM-disk drive (FILES/LOAD/SAVE/OPEN/INPUT#/PRINT#/KILL/NAME/SET/COPY/DSKF/BSAVE/BLOAD).

Research findings that shape the design:
- The PC-1600 has **no RAM vector** for files. `SCANMODS` (P0B0 07C5H) scans page-1 banks 1–7 at 4000H/6000H for a `43 16` module header. `FILE_I` (105FH) matches FCB+1..+4 against each module's device table (header +11H, 5-byte entries) and jumps to its file handler (+15H) with C = function and DE = FCB. The first match wins, in bank order.
- The MEP rev3 ROM (`~/Development/sharp/pc1600/disasm/mep3/PC1600-P1-B7.asm`) shows that a **file-level** driver works: it talks a byte protocol on port 90H and never uses sectors. We use it only as a guideline and write our own ROM.
- Bank 7, page 1 is where the real MEP sits on the expansion bus. It is open bus in `PC1600Memory::resolveConst`. Because bank 7 is after the CE-1600F's bank 5, `Y:` goes to the floppy when a CE-1600P is attached, and to us otherwise. `S3:` always reaches us.

User decisions:
- The card is attached on mount with an OFF/ON cycle and detached on unmount with an OFF/ON cycle.
- The floppy wins `Y:`.
- `INIT"S3:"` is refused.

## Design

### 1. Expansion bus: carry the bank number and widen the I/O window
- `Core/Connector/PC1600SystemBus.hpp`: replace `bool bank5` in `PC1600BusPins` with `uint8_t bank` (4–7), and change `readRom(offset, bank, out)` to match.
- `CE1600PCard::respondsToRead`: claim only banks 4 and 5 (`pins.bank == 5` replaces `bank5`). Update `ce1600p_tests`.
- `Core/PC1600/PC1600Memory.cpp` `read()`: send page-B banks 4–7 to the bus, not just 4 and 5. Update the resolveConst comment: bank 6 still has no content, and bank 7 is the expansion connector (MEP precedent).
- `readIOImpl`/`writeIO`: widen the bus window from 70H–8FH to 70H–9FH. Unclaimed ports still read FFH.
- This is a bus-level change, as `docs/Decisions.md` asks ("peripheral-ROM fetches use the generic bus path", "cards know only the bus").

### 2. Driver ROM (original work): `firmware/pc1600-hostdrive/hostdrive.asm`
- Z80, zasm dialect. Image: 16 KB, page-1 bank 7, module at 4000H; 6000H–7FFFH filled with FFH.
- Header, same layout as B3/B5/B7:
  - `43 16`
  - `JP reset`: for functions 0, 1, 3, 4 and 7 it sends RESET, and the host closes all handles.
  - `RET` (interrupt hook)
  - `SCF/RET` (disk IOCS)
  - `SCF/RET` (spare)
  - AUTORUN: return "not found"
  - DW devtab: `"S3",0,0,42H`, `"Y",0,0,0,43H`, `0`
  - DW 0 (no tokens, so INIT falls through to its normal "not here" error, which refuses it)
  - `JP filehandler`
- **Generic FCB round-trip protocol.** The ROM stays tiny and all the logic lives in testable C++:
  - **Port 91H write** = start command (function byte C). **Port 91H read** = status/ready. **Port 90H** = data stream.
  - **ROM → host:** DE (handle key, 2 bytes), `DEVNAME` (FC16H), FCB[00H..38H] (57 bytes, OTIR). For WRITE (15H) it also sends the 256 bytes at (DMA = FC46H).
  - **Host → ROM:**
    - status A (IOCS bits: 01 not found/end, 02 full, 04 EOF, 08 other, 10 unsupported, 20 no media) and ERL
    - the updated FCB[00H..38H], written back with INIR
    - a 2-byte payload length and the payload, copied to (DMA): a 256-byte record for READ, a 32-byte directory entry for SFIRST/SNEXT
    - 5 register bytes BC, E, HL, used by GETALLOC
  - The ROM sets ERL at F89BH and returns A.
  - SETDMA (1AH) is handled by `FILE_I` itself and never reaches us.
- Copy only the header, the handler contract and the status conventions from `PC1600-P1-B7.asm`, and follow `FDFILEHND`/`B5FILETAB` for the function list 0FH–23H.
- **Build:**
  - `tools/build_hostdrive_rom.sh` uses `$CALCU_ZASM` (default `~/Development/sharp/zasm/zasm -uwy`, as in `vscode/workspace/tasks.json`).
  - The assembled `PC1600-P1-B7-HOSTDRIVE.bin` and its `.lst` are committed next to the source, like the rom-dumper fixture.
  - `Qt6/CMakeLists.txt` bundles the `.bin` into Resources (next to the `roms/*.bin` glob, but from the tracked firmware dir, since `roms/` is gitignored).
  - `BundledRomCatalog.hpp` gets `loadPC1600HostDriveRom()`.

### 3. Host glue, Core only (std::filesystem, no Qt): `Core/Connector/HostDirectoryDrive.{hpp,cpp}`
- **Handles** are keyed by FCB address (DE). Each has the host path, mode and a `std::fstream`. SEARCH iterators are keyed by DE as well. `reset()`/unmount close everything.
- **Visible names:** regular, non-hidden files whose name fits 8.3 and uses only characters the ROM's file-spec parser accepts (verify against `FSPECINIT`/the name parser in B3B). Names are shown in upper case. If two host files collide case-insensitively on a case-sensitive file system, the second is hidden. Directories, dotfiles and long names are filtered out. Lookup maps an FCB name back to the real host name case-insensitively. New files are created in upper case.
- **Functions:**

| Function | Behaviour |
|---|---|
| OPEN 0FH | Mode FCB+5 (1 in / 2 out / 3 append). Fills FCB+14H..+28H like the floppy's OPEN (attribute, time, date, size at +25H). |
| CREATE 16H | Truncate or create. |
| READ 14H | Record n = FCB+30H..+32H (full 24-bit), 256 bytes, zero-padded. Past the end gives EOF 04H. |
| WRITE 15H | Write FCB+06H bytes (or 256) at the current position. |
| CLOSE 10H | Flush. |
| SFIRST/SNEXT 11H/12H | '?' pattern match on the FCB 8.3 form. Directory snapshot taken at SFIRST, sorted. |
| DELETE 13H | Honours protection; wildcards allowed. |
| RENAME 17H | New name at FCB+29H. Fails if the target exists. |
| SETATTR 1EH | P ↔ owner write permission. I ↔ `UF_HIDDEN` (chflags) on macOS, no-op elsewhere. |
| GETALLOC 1BH | Free space from `std::filesystem::space`, as BC = 512, E = cluster sectors, HL = clusters, capped to what `DSKF_B3` (B3 713CH) can multiply/print without overflow (check its math first). |
| GETLEN 23H | File size. |
| Everything else | 10H (unsupported), ERL 9EH. |
| No directory mounted | 20H (no media), ERL A0H. |

- **Directory entry (32 bytes):** 8.3 name, attribute, time word (h/m/s÷2) and date word from the host mtime in local time. The **year field is fixed at 6 (1986)**, as the ROM's own `FATTIME` does. FILES shows only mm/dd hh:mm, so no year is exposed. The first cluster is 0 and the size is 32-bit, clamped.
- **Host timestamps** come from the file system, so they are the PC clock's correct date and time, year included.
- File bytes are stored exactly as the PC-1600 sends them: tokenized BASIC header files, ASCII with CR LF + 1AH, and BSAVE headers all pass through unchanged. There is no line-ending conversion.

### 4. Card: `Core/Connector/PC1600HostDriveCard.hpp`
- A `PC1600ExpansionCard`:
  - ROM reads when `!pins.io && pins.bank == 7`.
  - Ports 90H/91H run the protocol state machine: collect the request bytes, call `HostDirectoryDrive::execute(fn, handle, fcb, data)`, then stream out the response buffer.
- It knows only the bus (Decisions.md).

### 5. Machine and controller wiring
- **`PC1600Machine`** (mutex-locked, like `attachCE1600P`): `attachHostDrive(rom, size, dir)`, `detachHostDrive()`, `hostDriveSetDirectory(dir)`, `hostDriveDirectory()`. The CE-1600P and the host drive coexist on the bus.
- **`Qt6/app/MachineController`:**
  - `mountHostDirectory(path)` attaches if needed and power-cycles OFF→ON, so SCANMODS finds bank 7. Reuse the mechanism the CE-1600P ROM swap uses for its OFF/ON cycle (see `swapCE1600PRom`, `MachineController.cpp:642`, and `PlotterController.cpp:36`). If already attached, it only swaps the directory and closes handles.
  - `unmountHostDirectory()` detaches and power-cycles.
  - After a machine rebuild (model switch / Reset All), re-attach if a path is mounted, as `insertSelectedDisk()` does for the floppy.
  - The mount is **not** persisted across launches.
- **`MainWindow::buildMenuBar()`** (`MainWindow.cpp:769ff`): add "Mount Directory…" and "Unmount Directory" after "Load Machine Code…".
  - They are enabled only for the PC-1600, and Unmount only while something is mounted. Its text includes the folder name.
  - Mount uses `QFileDialog::getExistingDirectory` from `AppSettings::openStartDir(...)`. Connect them next to the other File actions (around line 174).

### 6. Docs
- `docs/PC1600-Host-Drive.md`: the protocol, the ROM layout, the name filter, the date rule, what is supported.
- `docs/Decisions.md` entries:
  - bank 7 and the floppy winning `Y:`
  - INIT refused
  - year fixed at 1986
  - no line-ending conversion
  - the FCB round-trip protocol, deliberately not MEP-compatible
- `docs/PC1600-Host-Drive-Plan.md`: this plan.
- CHANGELOG entry.

## Phases (each committed to dev-0.6.0)
1. Bus generalization (bank number, I/O window), with CE-1600P/CE-1600F tests still green.
2. `HostDirectoryDrive`, with unit tests (`Core/tests/host_directory_drive_tests.cpp`) on a temp dir: 8.3 filter and collisions, directory-entry encoding and year, OPEN/READ/WRITE/CLOSE records incl. >32 KB, DELETE/RENAME/protect, GETALLOC cap, no-media status.
3. ROM source, build script, committed `.bin`, bundling and catalog loader.
4. Card and `PC1600Machine` attach/detach, with headless BASIC integration tests (`pc1600_host_drive_tests.cpp`, using the typing helpers from `pc1600_basictyper_tests`/`pc1600_machine_tests`):
   - `SAVE"S3:T.BAS"` → `NEW` → `LOAD"S3:T.BAS"` → `RUN`
   - `FILES"S3:"` shows host files, skips long names
   - `OPEN`/`PRINT#`/`INPUT#`
   - `KILL`, `NAME`, `SET ,"P"`
   - `COPY` between `S3:` and `S1:`
   - `DSKF("S3:")`
   - `BSAVE`/`BLOAD`
   - `INIT"S3:"` gives an error
   - `Y:` goes to the host drive without a CE-1600P and to the floppy with one
5. GUI menu and controller mount/unmount/re-attach.
6. Docs, Decisions.md, CHANGELOG, plan doc.

## Verification
- `cmake --build` and the full ctest suite. Existing PC-1600 boot, preset and timing tests must be unchanged when nothing is mounted.
- The new unit and integration tests from phases 2 and 4.
- Manual check in the app (quit via `calcu1600/quit`, `-ApplePersistenceIgnoreState YES`):
  1. Mount a folder that has a `.bas` file, a long-named file and a subfolder.
  2. `FILES"S3:"` lists only the 8.3 file, dated mm/dd.
  3. `SAVE"S3:NEW.BAS"`, then check on the Mac that the file exists with the current date/time.
  4. Edit a file on the Mac, then `LOAD` it.
  5. Unmount; `FILES"S3:"` then gives an error.
