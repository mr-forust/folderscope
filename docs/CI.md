# CI and release runner

FolderScope stays on the trusted shared runner because its CI image is large
and valuable to reuse. The workflow has no PR trigger; do not enable one until
the PR runner configuration is live and verified.

`.gitea/workflows/ci-release.yaml` runs on pushes to `main` and `fix/**`, on
version tags, and by manual dispatch. It has no pull-request trigger. Keep that
boundary until PR jobs use and verify the isolated runner. The VPS uses the
trusted `homelab` runner for build and release work.

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
preflight fails, use the reviewed VPS disk report to choose a specific cleanup
candidate. Do not remove the cached FolderScope image as a general cleanup.

To roll back these runner limits, revert the change to
`scripts/run-ci-container.sh`; the image fingerprint stays the same because
the CI Dockerfile is unchanged.
