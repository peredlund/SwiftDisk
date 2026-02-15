# SpaceScanner

Open-source, commercial-friendly disk space analyzer for Windows. The long-term goal is a WizTree-like UX using the NTFS MFT for very fast scans and a native Win32 + Direct2D UI.

## Status
- Core scanning engine and CLI are in place.
- NTFS MFT scanning is implemented for local NTFS volumes.
- GUI is planned (Win32 + Direct2D/DirectWrite).

## Build (CLI)
```powershell
cmake -S . -B build
cmake --build build --config Release
```

## Run
```powershell
build\Release\space_scanner_cli.exe scan --volume C
```

Note: reading the NTFS MFT requires direct volume access and may need elevated privileges.
This tool opens volumes read-only and refuses to scan if write access is requested.

## License
MIT. See `LICENSE`.
