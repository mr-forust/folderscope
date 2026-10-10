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
- Qt 6 with `Widgets`, `Concurrent`, `DBus`, and `LinguistTools`

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
curl -LO https://git.forust.xyz/api/packages/forust/arch/repository.key
sudo pacman-key --add repository.key
sudo pacman-key --lsign-key "$(gpg --show-keys --with-colons repository.key | awk -F: '/^fpr:/ {print $10; exit}')"
```

```ini
[folderscope]
SigLevel = Required
Server = https://git.forust.xyz/api/packages/forust/arch/$repo/$arch
```

Then install with `sudo pacman -Syu folderscope` (or `yay -S folderscope`).

### Release publishing requirements

The `ci-release` workflow uses the VPS `homelab` runner. The runner needs
Docker, Git, Python 3, curl, Node.js for checkout, and at least 5 GiB of free
space on both the workspace and Docker filesystems. The workflow builds a
cached Arch Linux image with CMake, Qt 6, Ninja, and makepkg; the VPS does
not need an Arch toolchain installed on the host. A cached image requires
at least 512 MiB of free space for each later build step. Builds run one compiler job at a time
to fit the VPS memory limit, including builds started by makepkg. Each container runs with the runner's UID and GID.

The lint tools use pinned versions in `scripts/tool-versions.env` and a
cache under `~/.cache/folderscope-ci`, following the homelab CI pattern.
Run `bash scripts/install-ci-tools.sh actionlint shellcheck yamllint` to
install them locally. Update the checkout commit and base image digest
only after checking the new upstream release and running the checks.

Arch publication requires the existing `FOLDERSCOPE_PACKAGE_TOKEN` Actions
secret with the `write:package` scope. A missing token, HTTP 401, or HTTP 409
stops publication and keeps the existing package. Release jobs run only
for version tags or explicit manual Arch publication from main. CI helpers
come from the workflow commit; the selected tag remains the package source.

Translations are Qt `.ts` files in `translations/`; CMake compiles and installs
their `.qm` files automatically. The application selects the system locale at
startup.

## Notes

FolderScope targets Linux only.

Some features from the Windows edition do not map directly to Linux, including
NTFS-only metadata and Windows shell integrations. The app uses native Linux
equivalents where available.
