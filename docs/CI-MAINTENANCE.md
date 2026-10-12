# CI maintenance

Light checks run on VPS. Compiler builds and image builds run on workstation.
PR jobs use isolated containers without host Docker access or deployment secrets.
Each check has a separate job. CI summary must fail when a required check fails.

## Tool downloads

Versions are defined in `scripts/tool-versions.env`.
Downloads must match a SHA256 in `scripts/download-checksums.txt`.
Unknown URLs and mismatched content fail before installation. To update a tool,
review its release, download the official artifact, verify its upstream checksum
when available, update the version and reviewed SHA256 together, and run CI.
Existing tools at the configured version are reused.

## Changes and rollback

Run CI on a pull request before merging. Main CI validates the merged commit.
Revert the focused CI commit to restore previous configuration. Reverting source
files does not restore runner configuration; use the Homelab runner backup procedure.
Do not prune shared Docker images or volumes to fix a failed job.

Automatic compilation remains disabled. `CI checks` validates workflow and shell files on VPS. Release builds remain manual.

## Required main checks

Main requires `CI checks / CI summary (pull_request_target)` on each PR. The summary requires all validation
jobs to succeed and allows only documented conditional image skips. Force push
to main is disabled. The owner's existing direct-push path and administrator
overrides remain available; normal changes should use checked PRs.

Runner post-task maintenance clears supported npm/uv download caches above
256 MiB each under the installer lock. Installed tools, runtimes, Rust caches,
Docker images and application data are outside that cleanup.

## Manual releases

Start `ci-release` from main. Set an existing `release_tag` contained in main.
Publication is opt-in: `publish_linux` creates Linux release assets and
`publish_arch` uploads the Arch package. Both default to false. The selected
tag is checked against VERSION and used as the release source. Concurrent
release runs for the same selected ref are serialized and never cancelled.
Release publication requires a separate live acceptance run; this CI maintenance
does not publish a release or enable automatic compilation.
