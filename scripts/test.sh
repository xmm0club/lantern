#!/usr/bin/env bash
set -uo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRATCH="${LANTERN_SCRATCH:-$PROJECT_ROOT/_scratch}"
SKIP_QEMU="${LANTERN_SKIP_QEMU:-0}"
BDF="$(sed -n '1p' "$PROJECT_ROOT/test/defaults.env")"
TOOL="$PROJECT_ROOT/_build/default/bin/tool.exe"
IMAGE="$SCRATCH/cli.img"
stages_run=0
stages_failed=0

log() { printf '\n[test] %s\n' "$*"; }

stage() {
  local name="$1"
  shift
  stages_run=$((stages_run + 1))
  log "$name"
  if "$@"; then
    printf '[test] %s: PASS\n' "$name"
  else
    printf '[test] %s: FAIL\n' "$name"
    stages_failed=$((stages_failed + 1))
  fi
}

cli_round_trip() {
  local input="$SCRATCH/pattern.bin"
  local output="$SCRATCH/readback.bin"
  head -c 65536 /dev/urandom > "$input" || return 1
  "$TOOL" write "$BDF" 8192 128 -i "$input" --image "$IMAGE" || return 1
  "$TOOL" read "$BDF" 8192 128 -o "$output" --image "$IMAGE" || return 1
  cmp "$input" "$output" || return 1
  printf '[test] 64 KiB written and read back byte for byte\n'
}

cli_identify() {
  "$TOOL" identify "$BDF" --image "$IMAGE" -v
}

cli_bench_polled() {
  "$TOOL" bench "$BDF" --image "$IMAGE" --pattern seq --size 8m --io-size 4096 \
    --queue-depth 32 --queues 1
}

cli_bench_interrupts() {
  "$TOOL" bench "$BDF" --image "$IMAGE" --pattern rand --rw write --size 8m --io-size 4096 \
    --queue-depth 16 --queues 3 --interrupts
}

cli_error_paths() {
  if "$TOOL" read "$BDF" 999999999 1 -o "$SCRATCH/nope.bin" --image "$IMAGE" 2>"$SCRATCH/error.txt"; then
    printf '[test] Reading past the end of the namespace unexpectedly succeeded\n'
    return 1
  fi
  grep -q "LBA out of range" "$SCRATCH/error.txt" || {
    printf '[test] Expected an LBA out of range diagnostic, got: %s\n' "$(cat "$SCRATCH/error.txt")"
    return 1
  }
  printf '[test] Out of range read reported as: %s' "$(cat "$SCRATCH/error.txt")"
}

qemu_stage() {
  "$PROJECT_ROOT/scripts/qemu.sh"
}

mkdir -p "$SCRATCH"
rm -f "$IMAGE"

stage "Build" dune build --root "$PROJECT_ROOT"
stage "C and OCaml suites" dune runtest --root "$PROJECT_ROOT" --force
stage "CLI identify" cli_identify
stage "CLI block round trip" cli_round_trip
stage "CLI error reporting" cli_error_paths
stage "CLI benchmark, polled" cli_bench_polled
stage "CLI benchmark, MSI-X" cli_bench_interrupts

if [ "$SKIP_QEMU" = "1" ]; then
  log "QEMU stage skipped because LANTERN_SKIP_QEMU=1"
else
  stage "QEMU guest with real VFIO" qemu_stage
fi

printf '\n[test] %d stages run, %d failed\n' "$stages_run" "$stages_failed"
if [ "$stages_failed" -eq 0 ]; then
  printf '[test] RESULT: PASS\n'
  exit 0
fi
printf '[test] RESULT: FAIL\n'
exit 1
