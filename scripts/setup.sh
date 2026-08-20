#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SKIP_QEMU="${LANTERN_SKIP_QEMU:-0}"

log() { printf '[setup] %s\n' "$*"; }

install_packages() {
  local wanted=(
    build-essential
    pkg-config
    ocaml-nox
    ocaml-dune
    ocaml-findlib
    libcmdliner-ocaml-dev
    zstd
  )
  if [ "$SKIP_QEMU" != "1" ]; then
    wanted+=(qemu-system-x86 busybox-static cpio kmod)
  fi

  local missing=()
  for package in "${wanted[@]}"; do
    dpkg-query -W -f='${Status}' "$package" 2>/dev/null | grep -q "install ok installed" || missing+=("$package")
  done

  if [ ${#missing[@]} -eq 0 ]; then
    log "All packages already present"
    return
  fi

  if [ "$(id -u)" -ne 0 ]; then
    log "Missing packages: ${missing[*]}"
    log "Re-run as root, or install them yourself"
    exit 3
  fi

  log "Installing: ${missing[*]}"
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -q
  apt-get install -y -q "${missing[@]}"
}

verify_toolchain() {
  log "OCaml $(ocamlc -version), dune $(dune --version), $(gcc --version | head -1)"
  ocamlfind query cmdliner >/dev/null
}

build() {
  log "Building"
  ( cd "$PROJECT_ROOT" && dune build )
}

main() {
  install_packages
  verify_toolchain
  build
  log "Environment ready"
  exec "$PROJECT_ROOT/scripts/test.sh" "$@"
}

main "$@"
