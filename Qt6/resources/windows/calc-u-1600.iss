; Inno Setup script for Calc-U-1600. Compiled headlessly in CI via
; ISCC.exe (see .github/workflows/build.yml's windows-x86_64 job) against
; the already-staged Qt6\dist folder (exe + Qt DLLs + MSVC runtime +
; resources\, produced by that job's "Stage standalone-runnable folder"
; step). Unsigned for now -- see PACKAGING-TODO.md.

[Setup]
AppName=Calc-U-1600
AppVersion=0.1.0
DefaultDirName={autopf}\Calc-U-1600
DefaultGroupName=Calc-U-1600
UninstallDisplayIcon={app}\Calc-U-1600.exe
OutputBaseFilename=Calc-U-1600-Setup
OutputDir=..\..\..
Compression=lzma2
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible

[Files]
Source: "..\..\dist\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{group}\Calc-U-1600"; Filename: "{app}\Calc-U-1600.exe"
Name: "{group}\Uninstall Calc-U-1600"; Filename: "{uninstallexe}"
; {commondesktop}, not {userdesktop}: PrivilegesRequired defaults to
; "admin" (needed for the {autopf}\Calc-U-1600 install dir above), and an
; elevated install's {userdesktop} would resolve to the elevated
; account's desktop, not the actual end user's.
Name: "{commondesktop}\Calc-U-1600"; Filename: "{app}\Calc-U-1600.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop icon"; GroupDescription: "Additional icons:"; Flags: unchecked
