# Changelog

All notable changes to FolderScope are documented here.

The project uses [Semantic Versioning](https://semver.org/). Release tags are
formatted as `vMAJOR.MINOR.PATCH`.

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

[0.2.0]: https://gitea.forust.xyz/forust/folderscope/releases/tag/v0.2.0
[0.2.1]: https://gitea.forust.xyz/forust/folderscope/releases/tag/v0.2.1
[0.2.2]: https://gitea.forust.xyz/forust/folderscope/releases/tag/v0.2.2
[0.2.3]: https://gitea.forust.xyz/forust/folderscope/releases/tag/v0.2.3
