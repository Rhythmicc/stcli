#!/usr/bin/env bash
set -euo pipefail
gcc -O2 -g0 -Wall -Wextra -Werror -DGPU_ID_SYCL -fstack-protector-strong \
  -D_FORTIFY_SOURCE=2 -c gpu_id.c -o gpu_id_sycl_main.o
icpx -std=c++17 -O2 -g0 -Wall -Wextra -Werror -fsycl \
  -fstack-protector-strong -Wl,-z,relro,-z,now \
  gpu_id_sycl_main.o gpu_id_sycl.cpp -ldl -o stcli-linux-x86_64-sycl
