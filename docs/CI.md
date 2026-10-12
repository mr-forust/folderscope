# CI and release runner

`CI checks` runs workflow and shell checks on VPS. Pushes to main and manual
runs from main also build, test, and smoke-test the GUI on `workstation-build`.
The summary requires the main build to succeed. PRs run isolated light checks;
they do not run the Docker build wrapper on the trusted host.

`.gitea/workflows/ci-release.yaml` remains manual. Publication is opt-in and
requires a validated release tag. Automatic CI does not publish releases.

`scripts/run-ci-container.sh` fingerprints `.gitea/ci/Dockerfile` and reuses
the matching `folderscope-ci` image. A cold image build requires 5,120 MiB
free on both the source filesystem and Docker filesystem. A warm run requires
512 MiB. Keep this image: rebuilding it installs the Arch toolchain from mutable package repositories and
costs substantial disk and time. The script builds CMake with one job and runs
the project container with one CPU, 768 MiB memory, and 512 processes.

The release workflow verifies the version and tag against `main` before
publishing Linux binaries or the Arch package. Publication secrets are scoped
to publish steps. There is no production deployment in this workflow.

No automatic image, BuildKit cache, or volume cleanup is configured. If a
preflight fails, use the reviewed workstation disk report to choose a specific cleanup
candidate. Do not remove the cached FolderScope image as a general cleanup.

To roll back these runner limits, revert the change to
`scripts/run-ci-container.sh`; the image fingerprint stays the same because
the CI Dockerfile is unchanged.

## Shared CI principles

Heavy compilation and packaging use `workstation-build`; lightweight checks in
other projects use `vps-light` or isolated `vps-pr`. Untrusted builds must use
`workstation-pr`, with no host cache mounts, Docker socket or secrets. This
project has automatic main builds and has no PR build trigger.
The workstation queue has capacity one. The build wrapper still limits each
container to one CPU and 768 MiB and uses one compiler job. It keeps disk
preflights and reuses only the Dockerfile fingerprint that matches the checkout.

The old VPS image was archived, verified with `docker load`, then removed with
user approval. Archive and checksum are on the local desktop (`gigaforust`) under:
`/home/forust/Documents/Codex/2026-10-11/files-pasted-by-the-user-vps/outputs/folderscope-ci-d2aa3d244a23bca4.tar.zst`.
SHA256: `8209a33ce3652c88d6ad4dc1b823cd1297d38f825c39f7ebb3c3752a42bf5a48`.
A restored local-desktop verification copy is tagged
`folderscope-ci:vps-backup-d2aa3d244a23bca4`; its RootFS layer digests match the
VPS source. Verify the archive checksum before loading it on another host.
The workstation reuses the image matching the checked-out Dockerfile fingerprint.
To disable automatic compilation again, revert the automatic build job and its
summary dependency in `ci-checks.yaml`; keep the light checks active. Restoring
the VPS image is unnecessary for workstation builds.

A second archive copy is retained on SSH host `workstation` in
`/home/forust/.local/share/gitea-ci-backups/`; its checksum is verified separately.
