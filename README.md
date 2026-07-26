# FolderScope

Fast, native file search for Linux with a Windows-style GUI.

FolderScope is a C++20 + Qt 6 desktop app for finding files across one or more
folders without indexing or background services. It is designed for fast
interactive searches, duplicate discovery, and detailed result inspection in a
single window.

## Highlights

- search across multiple root folders;
- filter by name patterns, size, timestamps, and file attributes;
- inspect duplicate files and files with matching names;
- optionally verify duplicate matches byte by byte in strict mode;
- search inside file contents, including text and hex;
- reuse a persistent, automatically invalidated cache for content matches and
  duplicate hashes;
- show folder summaries and duplicate groups;
- sort, resize, hide, and restore columns, including per-column header actions;
- export results to CSV, TSV/TXT, HTML, XML, and JSON;
- open files and folders, copy paths, and use context actions from the results
  table;
- run searches in parallel for responsive large-directory scans.

## Building

Requirements:

- CMake
- a C++20 compiler
- Qt 6 with `Widgets`, `Concurrent`, and `DBus`

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/folderscope
```

Run the automated tests:

```bash
ctest --test-dir build --output-on-failure
```

Install into the current system:

```bash
cmake --install build
```

## Notes

FolderScope targets Linux only.

Some Windows-specific SearchMyFiles features do not map directly to Linux,
including NTFS-only metadata and Windows shell integrations. The app uses the
native Linux equivalents where available.
