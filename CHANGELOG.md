# Changelog

All notable changes to FolderScope are documented here.

The project uses [Semantic Versioning](https://semver.org/). Release tags are
formatted as `vMAJOR.MINOR.PATCH`.

## [0.3.1] - 2026-10-09

### Fixed

- Applied result limits after duplicate classification and content filtering.
- Reported incomplete searches when file reads or path checks fail.
- Used Unicode case folding for text searches and rejected empty hex queries.
- Added file change times to cache validation.
- Saved exports atomically and checked write errors. Produced valid XML and
  protected spreadsheet cells from formula execution.
- Removed directory links without following their targets.
- Opened the desktop application chooser for Open With.
- Checked release refs and upload responses. Preserved existing release assets
  when a new upload fails.
- Declared Qt translation tools in Arch build dependencies.

- Preserved the duplicate display mode when editing search options.
- Counted each physical file once during content duplicate searches, including
  symbolic-link and hard-link aliases.
- Excluded duplicate candidates that changed after hashing and marked the search
  as incomplete.

### Changed

- Replaced numeric keeper priorities with clear copy status labels in the UI.
- Clarified duplicate group labels and display options. Added Russian translations.

## [0.3.0] - 2026-07-26

### Added

- Added persistent input history for folder and wildcard fields in Search
  Options.

### Changed

- Renamed the executable, desktop entry, icon, and application identity to
  FolderScope.
- Preserved existing settings and search profiles when migrating from the
  previous Linux application identity.
- Kept the base-folder browser button compact so the input field uses the
  available width.

## [0.2.5] - 2026-07-26

### Added

- Added an optional strict byte-by-byte verification mode for duplicate files.

### Changed

- Accelerated duplicate detection with size grouping, sampled block hashes, and
  full SHA-256 hashing only for remaining candidates.
- Normalized overlapping search roots and deduplicated file candidates so a
  path cannot be compared with itself.
- Strengthened persistent cache invalidation with nanosecond timestamps and
  filesystem identity.

## [0.2.4] - 2026-07-25

### Fixed

- Corrected result-table context menu positioning.
- Added validation for ISO date filters instead of silently accepting invalid
  input.
- Replaced the 2 GB size-filter limit with precise 64-bit inputs.
- Preserved visible table order when acting on multiple selected files.
- Made release preparation reject untracked files.

### Added

- Added automated tests for search primitives, large size inputs, and release
  safeguards.
- Added the test suite to the CI verification gate.

## [0.2.3] - 2026-07-25

### Added

- Added a persistent cache for duplicate hashes and file-content matches with
  automatic invalidation when files change.
- Added cache controls and a context menu on column headers for per-column and
  global automatic width.

### Changed

- Renamed duplicate result columns to `Duplicate Set` and `Keeper Priority`,
  with tooltips explaining which copy is preferred.

## [0.2.2] - 2026-07-25

### Fixed

- Scoped workflow YAML linting to the `.gitea` source directory.
- Combined CI and tag-gated release publishing into one workflow.

## [0.2.1] - 2026-07-25

### Fixed

- Removed the incompatible workflow-artifact upload step from Gitea Actions.
- Kept the release gate fail-closed: version validation, build, GUI smoke test,
  and YAML lint must all pass before Gitea release assets are published.

## [0.2.0] - 2026-07-25

### Added

- Initial Linux desktop release with C++20 and Qt 6.
- Advanced file search, content search, duplicate detection, summaries, and
  CSV/TXT/HTML/XML/JSON export.
- Configurable columns, persistent UI settings, context actions, and progress
  reporting.

[0.2.0]: https://git.forust.xyz/forust/folderscope/releases/tag/v0.2.0
[0.2.1]: https://git.forust.xyz/forust/folderscope/releases/tag/v0.2.1
[0.2.2]: https://git.forust.xyz/forust/folderscope/releases/tag/v0.2.2
[0.2.3]: https://git.forust.xyz/forust/folderscope/releases/tag/v0.2.3
[0.2.4]: https://git.forust.xyz/forust/folderscope/releases/tag/v0.2.4
[0.2.5]: https://git.forust.xyz/forust/folderscope/releases/tag/v0.2.5
[0.3.0]: https://git.forust.xyz/forust/folderscope/releases/tag/v0.3.0
