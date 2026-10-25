#!/bin/bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$ROOT/xnu/files/bsd" "$ROOT/tools/dada/tests/gpu-host.c" \
    "$ROOT/xnu/files/bsd/dev/dada_gpu_fw.c" "$ROOT/xnu/files/bsd/dev/dada_gpu_va.c" -o "$OUT/gpu-host"
"$OUT/gpu-host" "$@"
