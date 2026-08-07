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

## Packages

CPack produces native packages after a release build (the output is ignored by
Git):

```bash
cpack --config build/CPackConfig.cmake -B package
```

This creates `.deb` and `.rpm` packages. A Flatpak manifest is available at
`packaging/org.folderscope.FolderScope.yml`:

```bash
flatpak-builder --user --install --force-clean build-flatpak \
  packaging/org.folderscope.FolderScope.yml
```

For Arch, use `packaging/arch/PKGBUILD` with a release tarball named
`folderscope-VERSION.tar.gz`.

Published releases are also available through the Gitea Arch package registry.
Import its signing key and add the following to `/etc/pacman.conf`:

```bash
curl -LO https://gitea.forust.xyz/api/packages/forust/arch/repository.key
sudo pacman-key --add repository.key
sudo pacman-key --lsign-key "$(gpg --show-keys --with-colons repository.key | awk -F: '/^fpr:/ {print $10; exit}')"
```

```ini
[folderscope]
SigLevel = Required
Server = https://gitea.forust.xyz/api/packages/forust/arch/$repo/$arch
```

Then install with `sudo pacman -Syu folderscope` (or `yay -S folderscope`).

Translations are Qt `.ts` files in `translations/`; CMake compiles and installs
their `.qm` files automatically. The application selects the system locale at
startup.

## Notes

FolderScope targets Linux only.

Some Windows-specific SearchMyFiles features do not map directly to Linux,
including NTFS-only metadata and Windows shell integrations. The app uses the
native Linux equivalents where available.
