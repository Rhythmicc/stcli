#!/usr/bin/env bash
# Run in a fresh compile-env workspace containing only this public C source
# and the Zig compiler. No private upstream files required.
set -euo pipefail
ZIG_BIN="${STCLI_ZIG:-${GPU_ID_ZIG:-$(command -v zig || true)}}"
: "${ZIG_BIN:?Set STCLI_ZIG (or GPU_ID_ZIG) or ensure zig is in PATH}"

LINUX_FLAGS=(-O2 -g0 -Wall -Wextra -Werror -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wl,-z,relro,-z,now)

"$ZIG_BIN" cc -target x86_64-linux-gnu.2.17 "${LINUX_FLAGS[@]}" gpu_id.c -ldl -o stcli-linux-x86_64
"$ZIG_BIN" cc -target aarch64-linux-gnu.2.17 "${LINUX_FLAGS[@]}" gpu_id.c -ldl -o stcli-linux-aarch64
"$ZIG_BIN" cc -target loongarch64-linux-gnu.2.36 "${LINUX_FLAGS[@]}" gpu_id.c -ldl -o stcli-linux-loongarch64
"$ZIG_BIN" cc -target riscv64-linux-gnu.2.27 "${LINUX_FLAGS[@]}" gpu_id.c -ldl -o stcli-linux-riscv64
"$ZIG_BIN" cc -target x86_64-windows-gnu -O2 -g0 -Wall -Wextra -Werror gpu_id.c -o stcli-windows-x86_64.exe
