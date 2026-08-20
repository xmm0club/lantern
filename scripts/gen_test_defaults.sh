#!/usr/bin/env bash
set -euo pipefail

printf 'let bdf = %s\n' "\"$1\""
printf 'let selftest_image = %s\n' "\"$2\""
