# VPS CI status and image restore

Automatic CI and release runs are paused. `.gitea/workflows/ci-release.yaml`
has only `workflow_dispatch`; it has no push, tag or PR trigger. Keep it paused.
Manual jobs use the trusted `homelab` runner. Do not run the current main
workflow on the VPS: it still expects host build tools. The optimized build
wrapper and pinned tools are in `fix/finish-ci-and-packaging`, pending adoption.

That branch fingerprints `.gitea/ci/Dockerfile` and reuses the matching image.
Its cold build requires 5,120 MiB free; a warm run requires 512 MiB. It limits
the build to one compiler job and the container to one CPU, 768 MiB RAM and
512 processes. Restoring an old image only helps while its fingerprint matches.

## Approved image offload: 2026-10-11

The disabled project's image `folderscope-ci:d2aa3d244a23bca4` was removed
from the VPS with user approval. Its archive is stored on the workstation:

```text
/home/forust/Documents/Codex/2026-10-11/files-pasted-by-the-user-vps/outputs/folderscope-ci-d2aa3d244a23bca4.tar.zst
```

The adjacent `.sha256` file records SHA256
`8209a33ce3652c88d6ad4dc1b823cd1297d38f825c39f7ebb3c3752a42bf5a48`.
The archive passed `zstd -t` and a local `docker image load`; the restored
RootFS layer digests matched the VPS source. Offload released 2,132,893,696
bytes. Keep the archive outside Git. No application data or volumes were removed.

Before an approved manual build using the optimized workflow, restore from
the workstation (fish compatible):

```sh
cd /home/forust/Documents/Codex/2026-10-11/files-pasted-by-the-user-vps/outputs
sha256sum -c folderscope-ci-d2aa3d244a23bca4.tar.zst.sha256
zstd -dc folderscope-ci-d2aa3d244a23bca4.tar.zst | ssh vps docker image load
ssh vps docker image inspect folderscope-ci:d2aa3d244a23bca4
```

Stop if checksum verification fails. Loading needs disk for the unpacked image
(about 2.13 GB), then the warm run needs an additional 512 MiB reserve.
Restoring avoids a cold rebuild and does not enable automatic workflows.

No automatic image, BuildKit or volume cleanup is configured. Roll back the
offload by loading the archive. Keep workflow triggers disabled until the
optimized branch is reviewed and adopted; do not enable releases to test restore.
