# SwiftDisk

Open-source, commercial-friendly disk space analyzer for Windows. The long-term goal is a WizTree-like UX using the NTFS MFT for very fast scans and a native Win32 + Direct2D UI.

## Status
- Core scanning engine and GUI are in place.
- NTFS MFT scanning is implemented for local NTFS volumes.
- Native Win32 + Direct2D/DirectWrite UI is in place with clickable treemap navigation and in-app path selection.

## Build
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
build\Release\SwiftDisk.exe --path C:\
```

Note: `SwiftDisk.exe` is built to request administrator privileges on launch, so Windows will show a UAC prompt when you start it.
This is because reading the NTFS MFT requires direct volume access and need elevated privileges.
This tool opens volumes read-only and refuses to scan if write access is requested.

## License
MIT. See `LICENSE`.
