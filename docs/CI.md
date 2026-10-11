# CI and release runner

FolderScope stays on the trusted workstation runner because its CI image is large
and valuable to reuse. The workflow has no PR trigger; do not enable one until
the PR runner configuration is live and verified.

Automatic CI and release runs are paused. `.gitea/workflows/ci-release.yaml`
has only a manual `workflow_dispatch` trigger. It has no push, tag, or
pull-request trigger. Keep the workflow manual until there is enough workstation disk
space for the cold build and the isolated PR runner is verified. Manual jobs
use the trusted `workstation-build` runner.

`scripts/run-ci-container.sh` fingerprints `.gitea/ci/Dockerfile` and reuses
the matching `folderscope-ci` image. A cold image build requires 5,120 MiB
free on both the source filesystem and Docker filesystem. A warm run requires
512 MiB. Keep this image: rebuilding it installs the pinned Arch toolchain and
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
project keeps automatic triggers disabled and has no PR build trigger.
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
Before an approved manual run, check the Dockerfile fingerprint and matching
image; the original workstation tag may refer to another local build.
Do not assume a matching tag proves identical layers. No new FolderScope build
was started for this routing change. Rollback: revert this routing commit;
keep automatic triggers paused. Restoring the VPS image is optional and needs
space for its 2.13 GB unpacked data plus the 512 MiB warm-build reserve.

A second archive copy is retained on SSH host `workstation` in
`/home/forust/.local/share/gitea-ci-backups/`; its checksum is verified separately.
