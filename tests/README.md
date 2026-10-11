# NTFS extent verification

Configure with `cmake -S . -B build -DBUILD_TESTING=ON`, build Release, then run:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

The regression checks use synthetic Windows retrieval-pointer responses. They
cover fragmented mappings, continuation at extent boundaries, LCN zero,
truncated responses, invalid extent counts, VCN gaps/overlaps, zero progress,
sparse mappings and physical-offset overflow. Rejected pages must leave the
previously accepted map and continuation cursor unchanged.

For an actual read-only MFT scan, run this command in an administrator PowerShell:

```powershell
.\build\Release\ntfs_extent_tests.exe --scan C
```

Replace `C` with the desired NTFS drive letter. The command uses the production
volume reader and checks that it reads exactly the expected number of record
slots and finds the root FILE record. It does not write to the volume. This is
an explicit integration check, not part of the default CTest run.

Before release, compare full-drive and folder scans with the previous SwiftDisk
build on representative machines. A quiet test volume makes record counts and
size comparisons more useful; an active volume can change between scans. Include
a fragmented MFT if available, and check that scan failures are reported rather
than returning incomplete results as successful scans.
