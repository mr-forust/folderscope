#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 1 ]]; then
    echo "Usage: $0 COMMAND" >&2
    exit 2
fi
tools_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image="folderscope-ci:$(sha256sum "$tools_root/.gitea/ci/Dockerfile" | cut -c 1-16)"
min_free_mib=2048
if docker image inspect "$image" >/dev/null 2>&1; then
    min_free_mib=512
fi
"$tools_root/scripts/check-runner.sh" --min-free-mib "$min_free_mib" docker git
# Check the Docker filesystem as well as the source filesystem.
docker_root="$(docker info --format '{{.DockerRootDir}}')"
FOLDERSCOPE_CHECK_PATH="$docker_root" "$tools_root/scripts/check-runner.sh" --min-free-mib "$min_free_mib" docker
docker build --tag "$image" "$tools_root/.gitea/ci"
docker run --rm --user "$(id -u):$(id -g)" \
    --volume "$PWD:/work" --workdir /work \
    --env QT_QPA_PLATFORM=offscreen "$image" bash -euo pipefail -c "$1"
