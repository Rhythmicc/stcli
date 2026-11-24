# stcli 1.3.0（解得科技命令行工具）

本源码包采用 MIT 许可证，仅许可此独立工具，不包括求解器 SDK、显卡驱动或第三方编译器。工具无网络功能、无签名私钥，不生成授权。不带参数直接运行 `stcli` 输出帮助信息并正常退出（`0`）；使用 `stcli gpu-id`（可选 `--backend`）向标准输出打印每张卡的两行纯文本设备信息与 4 位校验码（查询失败或底层环境未配置时仅向标准错误输出诊断信息）；同时支持通过 `apply` / `list` 子命令检验、安装、替换并查看商城签发的 `ST.` 紧凑授权码。

> 注：自 `1.2.0` 起，原设备标识工具 `solved-tech-gpu-id` 正式更名为 **`stcli`**。

## 设备标识输出格式

每张卡独立输出两行（`#` 开头为平台/厂商与型号说明行，第二行为设备标识与 4 位 Crockford Base32 校验码）：

```text
# cuda | NVIDIA GeForce RTX 5060 Ti
cuda-a1b2c3d4-e5f60718-9a0b1c2d-3e4f5061 [7K3P]
# hip | hygon | Z100
hip-c3d4e5f6-0718293a-b1c2d3e4-f5061223 [Q92T]
```

## 授权安装与查看

三款解法器（`PanguLU`、`LinglongAMG`、`XianzhiKrylov`）统一按以下目录加载授权文件：

- 用户目录（默认）：`~/.solved-tech/<LibName>/*.key`
- 系统目录（需显式加 `--system`）：`/etc/solved-tech/<LibName>/*.key`

```sh
# 交互粘贴安装商城获取的 ST. 授权码（终端内回显 '*'，CRC 校验通过后自动完成）
./stcli-linux-x86_64 apply

# 直接传入授权码或通过管道导入
./stcli-linux-x86_64 apply 'ST....'
printf '%s' 'ST....' | ./stcli-linux-x86_64 apply

# 写入系统级目录 /etc/solved-tech/<LibName>/
sudo ./stcli-linux-x86_64 apply --system

# 查看已安装授权（支持 --system、--json）
./stcli-linux-x86_64 list
./stcli-linux-x86_64 list --json
```

`apply` 仅接受商城签发的 `ST.` 授权码，会自动校验包络与 `CRC-24` 校验和、重建标准 `PGL-LICENSE-1` `.key` 文件、拒绝演示占位数据，并按 `(product, gpu_serial)` 原子替换同一设备的已有授权（保留其余 GPU 的授权）。单卡典型授权码约 `115`~`125` 字符，4 卡同批约 `449` 字符。

## Linux 本机编译

需要 GCC 或 Clang、标准 C 库开发文件和对应 GPU 驱动。编译不需要
CUDA/MUSA/HIP/HCR/Level Zero/OpenCL SDK 头文件，运行通过动态加载系统驱动
查询设备。

```sh
cc -O2 -Wall -Wextra -Werror -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
  -Wl,-z,relro,-z,now gpu_id.c -ldl -o stcli
./stcli gpu-id
```

在目标 GPU 机器或 Slurm 分配到的 GPU 作业内运行，保持与求解器相同的设备可见性。每张卡输出的两行文本包含前导零与 4 位校验码（绑定平台、厂商、型号和设备标识）；可直接复制这两行到商城设备授权页粘贴导入。若执行失败，须先解决工具报错。

自动模式会扫描支持的后端。仅安装了 Level Zero loader、但没有初始化 Intel GPU
驱动时（`ZE_RESULT_ERROR_UNINITIALIZED / 0x78000001`），会跳过 Intel，
不会让已识别的 NVIDIA 等设备报告失败。显式 `--backend intel` 仍会报错；
其他初始化错误、已枚举设备的标识读取失败及重复 ID 仍会使查询失败。
没有可见 GPU 时仍返回失败。已知为 NVIDIA 的节点也可显式选择：

```sh
srun --immediate=5 -p a100 -N1 -n1 --mem=128M -t 00:01:00 \
  ./stcli-linux-x86_64 gpu-id --backend cuda
```

## 跨平台构建

`build.sh` 使用 Zig 交叉编译多个 Linux 指令集与 Windows 目标（所有 Linux 版本均通过运行时动态加载 `dlopen` 兼容全部 GPU 后端）：

- `stcli-linux-x86_64`：`x86_64-linux-gnu.2.17`（glibc 2.17+）
- `stcli-linux-aarch64`：`aarch64-linux-gnu.2.17`（glibc 2.17+，通用 ARM64）
- `stcli-linux-loongarch64`：`loongarch64-linux-gnu.2.36`（glibc 2.36+，龙芯新世界）
- `stcli-linux-riscv64`：`riscv64-linux-gnu.2.27`（glibc 2.27+）
- `stcli-windows-x86_64.exe`：`x86_64-windows-gnu`（NVIDIA，尚待 Windows 实机验证）

安装 Zig 后确保其在 `PATH` 中，或设置 `STCLI_ZIG`（兼容 `GPU_ID_ZIG`）为可执行文件路径，再运行 `bash build.sh`。使用不同版本或源码改动的编译器会改变构建产物哈希。

## Intel 与砺算

`--backend intel` 通过 Level Zero（`libze_loader.so`）读取 Intel GPU 的 128 位
设备 UUID。Level Zero 按低字节在前返回 UUID，工具按高字节在前整理成
`00000000-0000-0003-0000-0000e20b8086` 这种与 PanguLU 授权一致的规范形式；
UUID 全零时按查询失败处理。

`--backend lisuan` 通过 Khronos OpenCL（`libOpenCL.so.1`）枚举砺算 GPU，读取
`CL_DEVICE_UUID_KHR` 并生成 `lisuan-` 前缀标识。当前砺算运行时未暴露非零
UUID 时，工具沿用 PanguLU OpenCL-lisuan 交付版的行为，输出全零 UUID 的
`lisuan-00000000-00000000-00000000-00000000`。

`build-sycl.sh` 与 `gpu_id_sycl.cpp` 是历史遗留的独立 SYCL 适配，仅供检查；
主工具的 Intel 路径不需要 oneAPI 头文件或 `icpx`。龙芯（OpenCL LACm）暂未加入。

## 输出与安全

`--backend cuda|musa|hip|hcr|intel|lisuan` 可以显式选择驱动。工具只输出纯文本（每卡 `# <backend> | <name>` 或 `# hip | <vendor> | <name>`，加 `<license_id> [<4位校验码>]`），无隐藏二进制字段、无自动上传；提交到商城时，校验码用于核对型号与 ID 完整一致（不作为密码学防伪证明），ID 用于授权，平台类别与型号仅用于授权页展示和统计汇总。

NVIDIA 优先使用驱动读取的硬件串号，其他平台按当前 SDK 的 UUID / PCI 规则生成标识，HCR 使用 hc- 前缀，Intel 使用规范形式 UUID，砺算使用 lisuan- 前缀。授权校验由求解器库在实际硬件上执行。

## 本地模拟驱动检查与授权测试

仅在 Linux 的临时目录中运行，不能将模拟驱动装入系统驱动目录。需要 Python 3 和 GCC：

```sh
STCLI_BINARY="$PWD/stcli" python3 test_helper.py
STCLI_BINARY="$PWD/stcli" python3 test_license_apply.py
STCLI_BINARY="$PWD/stcli" python3 test_license_paste.py
```

模拟测试不替代目标硬件上的授权兼容性验证。源码包没有用户设备数据、内部逆向记录、签名密钥或管理权限。

## HIP 厂商信息（1.1.5）

Linux 按 HIP 返回的 PCI Bus ID 读取 `/sys/bus/pci/devices/<BDF>/vendor`
及 `device`，可选输出 `vendor`、`vendor_source`、`pci_vendor_id`、
`pci_device_id`。PCI 厂商号 `0x1d94` 识别为海光，`0x1002` 识别为 AMD，
优先于型号名称。Z100 实机验证为 `1d94:54b7`（hydcu）。
无法读取或无法识别 PCI 厂商号时，尝试识别 HYGON/DCU/海光/Z100 或
AMD/Radeon/Instinct 型号；否则输出 `hip_unknown`，网页显示“AMD / 海光”，
表示厂商尚未区分。厂商信息缺失不会影响查询完成状态。

新增字段仅为展示信息，报告可由用户编辑，不构成硬件真实性证明。
UUID/PCI 授权标识生成、`--plain` 输出及旧 JSON schema 保持兼容。

## 1.1.6：活动硬件核验信息

HIP 设备输出新增 `pci_subsystem_vendor_id`、`pci_subsystem_device_id` 、`vram_bytes`、`board_part_number` 和 `vbios_version`，来自对应 PCI 设备的 Linux sysfs。不可读时返回空字符串或 0，不猜测具体型号，不改变 `license_id` 或 `--plain` 输出。BW 是驱动上报的名称，不能据此认定为 BW1000；活动后台需使用经厂商资料或实机核准的完整硬件规则。

JSON 是用户本机上报的设备信息，不是远程硬件防伪证明。未取得可区分 BW1000/BW100 的核准数据前，活动保持草稿。

## 1.1.7：Linux 驱动生命周期修复

Linux 查询后不再提前卸载 GPU 驱动动态库，让它们保持加载到进程退出，避免 HIP/DTK 在线程局部对象退出清理时调用已卸载的代码。正常的 C/C++ 退出清理和输出写入检查保留；未捕获或忽略段错误，设备标识与 JSON schema 不变。针对外部清理回调的模拟回归检查通过不代表特定 BW 主机实测通过。Windows 的 FreeLibrary 行为未改。

## 1.1.8：AMD Vega / ROCm 4.5.2 支持、底层环境诊断与失败不输出 JSON

- **失败不输出 JSON**：当驱动库缺失、权限不足、设备可见性过滤、缺少硬件唯一 ID 或出现重复 ID 时，仅向 `stderr` 输出诊断说明并返回退出码 `1`，不再输出 `complete:false` 的空 JSON。
- **Linux 底层环境配置诊断**：查询不到可用 GPU 时，自动检查 `/sys/bus/pci/devices`（识别 AMD `0x1002`、海光 `0x1d94`、NVIDIA `0x10de`、摩尔线程 `0x1ed5`、沐曦 `0x9999`、Intel `0x8086`）、`/dev/kfd` 存在性与读写权限、`/dev/nvidiactl` 权限以及 `ROCR_VISIBLE_DEVICES` / `HIP_VISIBLE_DEVICES` / `CUDA_VISIBLE_DEVICES`，明确指出缺失的运行时库、权限或环境变量问题。
- **ROCm 4.5.2 与 AMD Vega Frontier 硬件唯一 ID**：
  - 扩展 HIP 库探测：依次尝试 `libamdhip64.so` 及 `.so.7/.6/.5/.4`、`ROCM_PATH`/`HIP_PATH` 下的 `lib/` 与 `hip/lib/`，以及 `/opt/dtk`、`/opt/rocm/lib`、`/opt/rocm/hip/lib`、`/opt/rocm-4.5.2/hip/lib`。
  - 当运行时未导出 `hipDeviceGetUuid`（如 ROCm 4.5.2 `libamdhip64.so.4`）或返回无效/占位符（全零 / `GPU-XX`）时，在 Linux 上读取内核 `amdgpu` sysfs 的 64 位硬件唯一标识 `/sys/bus/pci/devices/<BDF>/unique_id`（次选 KFD 拓扑 `/sys/class/kfd/kfd/topology/nodes/*/properties` 的 `unique_id`），按与 ROCm 5.2+/7.2.1 `hipDeviceGetUuid` 完全一致的 16 字节小写 ASCII 十六进制编码生成 `hip-xxxxxxxx-xxxxxxxx-xxxxxxxx-xxxxxxxx`（例如 `GPU-0213f2e6924a1164` → `hip-30323133-66326536-39323461-31313634`）。
  - **取消 HIP 的 PCI 地址回退**：不再回退到 `hip-0000:xx:00`（PCI 槽位地址无法唯一标识物理显卡）；若无法获取硬件唯一 ID 则明确报错退出。
  - 厂商名称兜底新增 `VEGA`（PCI 厂商号 `0x1002` 仍优先）。
