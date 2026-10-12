#!/usr/bin/env bash
# Installs the pinned CI tools into "$TOOLS_DIR/bin" and echoes that directory
# on stdout, so callers can do:
#
#   export PATH="$(bash scripts/install-ci-tools.sh actionlint shellcheck):$PATH"
#
# Versions come from tool-versions.env next to this script. A tool already
# installed at the pinned version is left alone.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tool-versions.env
. "$here/tool-versions.env"

TOOLS_DIR="${TOOLS_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/folderscope-ci}"
BIN_DIR="$TOOLS_DIR/bin"
mkdir -p "$BIN_DIR"
# A runner may accept overlapping workflows even though each workflow is sequential.
exec 9>"$TOOLS_DIR/install.lock"
flock -w 300 9
export UV_TOOL_DIR="$TOOLS_DIR/uv-tools"
export UV_CACHE_DIR="$TOOLS_DIR/uv-cache"
# The just-installed tools must resolve inside this script too: callers only
# prepend BIN_DIR to PATH after the script exits, so a bare `uv` below would
# miss the binary install_uv just placed (exit 127 on a clean runner).
export PATH="$BIN_DIR:$PATH"

arch="$(uname -m)"
# actionlint uses Go architecture names; ShellCheck and uv use uname names.
case "$arch" in
  x86_64 | amd64)
    goarch=amd64
    sharch=x86_64
    ;;
  aarch64 | arm64)
    goarch=arm64
    sharch=aarch64
    ;;
  *)
    echo "install-ci-tools: unsupported architecture: $arch" >&2
    exit 1
    ;;
esac

fetch() {
  bash "$here/download-verified.sh" "$1" "$2"
}

# resolve <command>
# Absolute path to use for invoking a tool: the copy in BIN_DIR when present,
# otherwise the name for PATH lookup. Every version check and every in-script
# invocation goes through this, so a tool missing from both places reads as
# "not installed" instead of dying with 127 under `set -e`.
resolve() {
  if [ -x "$BIN_DIR/$1" ]; then
    printf '%s' "$BIN_DIR/$1"
  else
    printf '%s' "$1"
  fi
}

# installed_version <command>
# Prints the version of an already-installed tool, or nothing. Each tool spells
# its version flag differently, hence the case.
installed_version() {
  local bin out
  bin="$(resolve "$1")"
  if ! command -v "$bin" >/dev/null 2>&1; then
    return 0
  fi
  out="$("$bin" --version 2>/dev/null | head -1 || true)"
  printf '%s' "$out"
}

# at_version <command> <expected>
at_version() {
  local version expected="${2#v}"
  version="$(installed_version "$1")"
  if [[ "$version" =~ (^|[^0-9.])v?([0-9]+(\.[0-9]+)+) ]]; then
    [ "${BASH_REMATCH[2]}" = "$expected" ]
  else
    return 1
  fi
}

install_shellcheck() {
  if at_version shellcheck "${SHELLCHECK_VERSION}"; then
    return 0
  fi
  local tmp
  tmp="$(mktemp -d)"
  fetch "https://github.com/koalaman/shellcheck/releases/download/v${SHELLCHECK_VERSION}/shellcheck-v${SHELLCHECK_VERSION}.linux.${sharch}.tar.xz" \
    "$tmp/shellcheck.tar.xz"
  tar -xJf "$tmp/shellcheck.tar.xz" -C "$tmp" --strip-components=1 "shellcheck-v${SHELLCHECK_VERSION}/shellcheck"
  install -m 0755 "$tmp/shellcheck" "$BIN_DIR/shellcheck"
  rm -rf "$tmp"
}

install_uv() {
  if at_version uv "${UV_VERSION}"; then
    return 0
  fi
  local tmp
  tmp="$(mktemp -d)"
  # uv release tags carry no leading v, unlike every other tool installed here.
  fetch "https://github.com/astral-sh/uv/releases/download/${UV_VERSION}/uv-${sharch}-unknown-linux-gnu.tar.gz" \
    "$tmp/uv.tar.gz"
  tar -xzf "$tmp/uv.tar.gz" -C "$tmp" --strip-components=1 "uv-${sharch}-unknown-linux-gnu/uv"
  install -m 0755 "$tmp/uv" "$BIN_DIR/uv"
  rm -rf "$tmp"
}

install_uv_tool() {
  # <package> <pinned version>
  if at_version "$1" "$2"; then
    return 0
  fi
  install_uv
  UV_TOOL_BIN_DIR="$BIN_DIR" "$BIN_DIR/uv" tool install --force "$1==$2" >/dev/null
}

install_yamllint() {
  install_uv_tool yamllint "${YAMLLINT_VERSION}"
}

install_actionlint() {
  if at_version actionlint "${ACTIONLINT_VERSION}"; then
    return 0
  fi
  local tmp
  tmp="$(mktemp -d)"
  fetch "https://github.com/rhysd/actionlint/releases/download/v${ACTIONLINT_VERSION}/actionlint_${ACTIONLINT_VERSION}_linux_${goarch}.tar.gz" \
    "$tmp/actionlint.tar.gz"
  tar -xzf "$tmp/actionlint.tar.gz" -C "$tmp" actionlint
  install -m 0755 "$tmp/actionlint" "$BIN_DIR/actionlint"
  rm -rf "$tmp"
}

for tool in "${@:-actionlint}"; do
  case "$tool" in
    actionlint) install_actionlint ;;
    shellcheck) install_shellcheck ;;
    yamllint) install_yamllint ;;
    *) echo "Unknown CI tool: $tool" >&2; exit 2 ;;
  esac
done
printf '%s\n' "$BIN_DIR"
