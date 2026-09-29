# FILEX on the PC-1600

FILEX (H. Richter, `FILEX.zip` in the "Maschine" category of
sharp-pc-1600.de) is a Z-80 drive and file browser. It shows the registered
drives, directories as text or graphics, drive and module details, and
fragmented files, and it can print a directory to a file. It treats `S3:` as
a MEP rev3 USB drive, so on the host drive it also browses subfolders
([PC1600-Host-Drive.md](PC1600-Host-Drive.md)).

This guide follows FILEX 1.31 (`FILEX13E.BIN`, English; `FILEX13G.BIN` is
German). It was worked out from the program itself, and 1.4x additions are
listed at the end. Keys are PC-1600 keys; the host keys are in
[Keyboard-Mapping.md](Keyboard-Mapping.md): MODE is Tab, RCL is Home, CL is
Forward Delete, ⇕ is the `RSV` key (Scroll Lock / Insert, or the on-screen
key), and OFF is on the on-screen faceplate only.

## Starting

**File ▸ Load Machine Code…** with `FILEX13E.BIN`. The header loads it at
`&C0C5` in internal RAM. Then, in PRO mode, reserve the area and start it:

```
NEW "S0:",&2782
CALL &C0C5
```

## Registered drives

The first screen shows up to four drives, one per column: **F1** `S1:`,
**F2** `S2:`, **F3** `S3:`, **F4** `X:` (floppy). A column with `---` has no
drive. A function key opens that drive's menu.

- **MODE** quits FILEX back to BASIC.
- **OFF** switches off; the calculator resumes in FILEX.

## Drive menu

| Key | Screen |
|---|---|
| F1 | Text directory |
| F2 | System: drive and module details; for `S3:` "USB module", place "System bus", version rev3 |
| F3 | Graphical directory |
| F4 | Print the directory to a file ("Save with drive and name:", CL deletes the line) |
| F5 | Fragmented files |
| F6 | Help (P shows the password) |
| MODE | Back to "Registered drives" |

## Text directory

| Key | Action |
|---|---|
| ↑ / ↓ | Previous / next entry |
| ← / → | Other column |
| I | Information on the entry (file: size, type, date; folder: its files and subfolders) |
| MODE | Back; on `S3:` inside a folder, one folder up first |

On `S3:` only:

| Key | Action |
|---|---|
| ⇕ | Switch between the folder view and the file view |
| ENTER | Open the folder under the cursor (in the folder view) |
| RCL | Show the current path; RCL again closes it |
| CL | Back to the top folder, file view |

The other screens (System, Graphical directory, Fragments, Help) go back
with **MODE**.

## FILEX 1.4x

The help screen of 1.4x lists more keys in the text directory: **S** split
screen, **C** copy, **K** delete, **N** rename, **P** print to file, **R**
refresh `S3:`, **W** show the password, and **BREAK** (ON) back to the menu.
