# SpaceScanner

Open-source, commercial-friendly disk space analyzer for Windows. The long-term goal is a WizTree-like UX using the NTFS MFT for very fast scans and a native Win32 + Direct2D UI.

## Status
- Core scanning engine and CLI are in place.
- NTFS MFT scanning is implemented for local NTFS volumes.
- GUI shell is now in place (Win32 + Direct2D/DirectWrite) with clickable treemap navigation and in-app path selection.

## Build (CLI)
```powershell
cmake -S . -B build
cmake --build build --config Release
```

## Generate Visual Studio 2026 Solution
Use the Visual Studio 2026 CMake generator to create a `.sln` file and Visual Studio project files in the `build` directory:

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
```

Then build from the command line:

```powershell
cmake --build build --config Release
```

Or open the generated solution in Visual Studio 2026:

```powershell
cmake --open build
```

## Run
```powershell
build\Release\space_scanner_cli.exe scan --path C:\
build\Release\space_scanner_cli.exe scan --path E:\Video
build\Release\space_scanner_cli.exe scan --path C:\ --treemap-out c:\temp\space-treemap.html
build\Release\space_scanner_gui.exe --path C:\
```

Note: `space_scanner_gui.exe` is built to request administrator privileges on launch, so Windows will show a UAC prompt when you start it.
This is because reading the NTFS MFT requires direct volume access and need elevated privileges.
This tool opens volumes read-only and refuses to scan if write access is requested.

## License
MIT. See `LICENSE`.
