SquashFS 7z Plugin

📜 Description

SquashFS 7z Plugin lets you work with OGSR Engine SquashFS archives (`.sq`, `.sq_01`…) in 7-Zip like normal archives. You can browse files, edit content, and build new mod archives without mksquashfs.

📋 Features
- Open, extract, and create SquashFS 4.0 archives with LZ4 compression directly with 7-Zip
- Edit archives in place (add, replace, rename, delete files and folders)
- Unchanged files are copied without recompression, so editing a multi-gigabyte archive takes seconds
- Builds archives like `mksquashfs -comp lz4 -Xhc`: 128 KiB blocks, tail packing into fragments, duplicate files stored once
- `sq (OGSR)` format follows the [OGSR Engine file system](https://github.com/OGSR/OGSR-Engine/wiki/File-System) rules: a top-level `gamedata` folder is dropped, since the engine mounts the archive root as `gamedata\`
- SquashFS images with other compression (gzip, xz, zstd) are left to the built-in 7-Zip handler

📦 Installation
1. Download the latest release `.dll` file from GitHub
2. Pick the DLL that matches your 7‑Zip install:
   - `SQ_x64.dll` for 64‑bit 7‑Zip (most installs)
3. Copy the DLL into your 7‑Zip `Formats` directory (e.g., `C:\Program Files\7-Zip\Formats` or `C:\Program Files\7-Zip-Zstandard\Formats`). Create the `Formats` folder if it isn't there.

Check: `7z i` lists the `sq (OGSR)` and `sq` formats.

🛠 Build

Requirements: Visual Studio 2022 or 2026 with the C++ workload (its bundled CMake is used if `cmake` is not on PATH). The 7-Zip 26.05 and LZ4 1.10.0 sources are included in `third_party`.

```
build.cmd
```

or

```
cmake -S . -B _build/x64 -G "Visual Studio 18 2026" -A x64
cmake --build _build/x64 --config Release
```

Win32 build: replace `-A x64` with `-A Win32` and use `_build/Win32`.

Tested supported generators:
- Visual Studio 18 2026

Result
- `_build/x64/bin/Release/SQ_x64.dll`
- `_build/Win32/bin/Release/SQ_x86.dll`

🧭 Usage in 7‑Zip

Formats:

| | `sq (OGSR)` | `sq` |
|---|---|---|
| Opens by default | `.sq`, `.sq_01` … `.sq_09` | `.sqfs` |
| Top-level `gamedata` folder | dropped: `gamedata\config\x.ltx` → `config\x.ltx` | kept as is |
| File name encoding | CP1251 (what the engine looks up) | UTF-8 |
| Compression | always LZ4 HC, 128 KiB blocks, fragments | from parameters |

Both formats read archives the same way; they differ only when writing. Use `sq (OGSR)` for game archives.

Parameters:
- `level=0..12` (default 9): 0 stores data, 1–2 use LZ4, 3–12 use LZ4 HC with that level
  - `sq` also takes the 7-Zip compression level (`-mx`) the same way
- `block=4k..1m` (default 128k, `sq` only): block size, a power of two
  - Existing archive: keeps its block size unless set
- `frags=on|off` (default on, `sq` only): pack small files into fragment blocks

Examples:
- 7z.exe: `7z a "-tsq (OGSR)" mymod.sq_01 gamedata`
- 7z.exe: `7z a "-tsq (OGSR)" mymod.sq_01 .\gamedata\* -mlevel=12`
- 7z.exe: `7z a -tsq image.sqfs .\folder\* -mblock=256k -mfrags=off`
- GUI: Add to archive → Archive format `sq (OGSR)`; add the `gamedata` folder or its contents

Archive properties shown in 7‑Zip:
- Method: `LZ4` or `LZ4 HC`
- Cluster Size: block size

Notes:
- File modes: 0644 for files, 0755 for folders, owner root; files taken from an opened archive keep their modes
- Symbolic links, special files and xattrs are not supported (game archives have none)

📂 Layout
- `CPP/7zip/Archive/Sq` - plugin sources
  - `SqIn.cpp` - reading and extraction
  - `SqOut.cpp` - writing: data and fragments, then inode, directory, fragment and id tables; the superblock is written last
  - `SqRegister.cpp` - registration of the `sq (OGSR)` and `sq` formats
- `third_party/7zip` - upstream 7‑Zip 26.05 sources
- `third_party/lz4` - LZ4 1.10.0 library

📄 License
- Plugin-specific code: MIT (see LICENSE)
- Third-party code keeps its own licenses (see NOTICE)
