# NTFS extent and size verification

Configure with `cmake -S . -B build -DBUILD_TESTING=ON`, build Release, then run:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

The regression checks use synthetic Windows retrieval-pointer responses. They
cover fragmented mappings, continuation at extent boundaries, LCN zero,
truncated responses, invalid extent counts, VCN gaps/overlaps, zero progress,
sparse mappings and physical-offset overflow. Rejected pages must leave the
previously accepted map and continuation cursor unchanged.

The same executable also checks logical size versus allocation for resident,
ordinary, compressed, sparse, empty and preallocated data, continuation segments,
nested folder totals, and mode-dependent treemap selection and area. These default
regression checks are entirely in memory and do not open or write a volume.

To compare both sizes with Windows metadata for every file in a small, quiet
test folder, run from an administrator PowerShell on NTFS:

```powershell
.\build\Release\ntfs_extent_tests.exe --sizes "C:\YourTestFolder"
```

This exercises the production MFT parser and scanner. The following variant
exercises the ordinary directory scanner and does not require administrator
access:

```powershell
.\build\Release\ntfs_extent_tests.exe --directory-sizes "C:\YourTestFolder"
```

Both commands only read existing files and check folder totals as well as
individual files. Use a dedicated folder containing ordinary uncompressed data,
a 5 KiB file, an empty file, a tiny resident file, compressible text compressed
with `compact /c`, and a sparse file with a large unwritten region. Creating
these fixtures is a separate, deliberate file-writing step; neither verification
command creates them. Avoid links and alternate streams in this basic test set.

In the GUI, scan the test folder and toggle the Size on disk checkbox. Unchecked
uses Size; checked uses Size on disk. Check
that totals, breadcrumb sizes, tile sizes and ordering update immediately; both
values appear in tile and breadcrumb tooltips; resident and fully sparse files
consume no area in disk mode; and the choice survives a rescan and app restart.
Compare file Properties in Explorer, then decompress the compressed file and
rescan: logical size should stay constant while allocation increases.

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
