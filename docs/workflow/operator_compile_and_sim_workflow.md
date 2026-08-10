# 算子编译与仿真执行流程

本文档以 `group_token_old` 算子为例，描述 SuperNPUBench 算子从源码到仿真的完整执行过程和原理。

---

## 整体流程图

```
算子源码 (.cpp + .hpp)
       │
       │  ① 编译 (Linx clang-15)
       ▼
算子 ELF (PTO BlockISA 可执行文件)
       │
       ├──→ ② gfrun 功能仿真 → R2=0 (PASS) / R2=1 (FAIL)
       │
       └──→ ③ gfsim 时序仿真 → Total Cycles + PMU 统计
```

---

## ① 编译：源码 → ELF

### 执行命令

```bash
export COMPILER_DIR=/mnt/workspace/gitCode/cann/Dev-experience/v300/linx-toolchain-build/output/linx_blockisa_llvm_musl/bin
cd /mnt/workspace/gitCode/cann/Dev-experience/v300/SuperNPUBench/benchmark/one-level-arch/test/kernel/group_token_old
make TESTCASE=group_token_old PLAT=linx
```

### 原理

`Makefile` 引入 `test/common/Makefile.common`，该公共 Makefile 根据 `PLAT=linx` 配置 Linx 工具链：

| Makefile 变量 | 值 | 作用 |
|---|---|---|
| `CC` / `CXX` | `$(COMPILER_DIR)/clang` / `clang++` | Linx clang-15 编译器 |
| `CC_O` | `-c -mlxbc -fenable-matrix -O2 -mllvm -enable-all-vector-as-tilereg=true ...` | 编译选项 |
| `CXX_VER` | `-std=c++20` | C++ 标准 |
| `DEFINES` | `-D__linx -DENABLE_TENSOR_INSTR` | 宏定义（选择 linx 后端 + 启用 tile 指令） |
| `INCLUDE` | `-I$(ROOT)/include -I$(ROOT)/kernels ...` | 头文件搜索路径 |

编译分两步：

**步骤 1：编译 `.cpp` → `.o`（目标文件）**

```bash
clang++ -c -mlxbc -fenable-matrix -O2 -mllvm -enable-all-vector-as-tilereg=true \
  -mllvm -linxv5-enable-HL-Inst-Opt=true \
  -mllvm -linxv5-enable-dim-opt=true \
  -mllvm -linxv5-enable-ldst-bridge=false \
  -mllvm -linxv5-enable-continuous-mem-opt=true \
  -mllvm -linxv5-enable-tile-clock-hand=false \
  -mllvm -linxv5-enable-simt-clock-hand=true \
  -mllvm -enable-misched=false \
  -std=c++20 \
  -I.../include -I.../kernels -I.../test/common \
  -D__linx -DENABLE_TENSOR_INSTR \
  group_token_old.cpp -o group_token_old.o
```

关键编译选项含义：
- `-mlxbc`：生成 Linx BlockISA 指令（块结构 ISA）
- `-fenable-matrix`：启用 tile 寄存器 / 矩阵指令支持
- `-mllvm -enable-all-vector-as-tilereg=true`：把所有向量寄存器映射为 tile 寄存器
- `-mllvm -linxv5-enable-*`：Linx V5 后端优化开关
- `-D__linx`：选择 `jcore/` 后端的 tile op 实现
- `-DENABLE_TENSOR_INSTR`：启用 `blk_tload`/`blk_matmul` 等硬件内建

**步骤 2：链接 `.o` + `_start.s` → `.elf`（可执行文件）**

```bash
clang++ -nostartfiles _start.s group_token_old.o -o group_token_old.elf
```

- `-nostartfiles`：不使用标准 CRT 启动文件，用仓库自带的 `_start.s`
- `_start.s`：bare-metal 入口代码，调用 `main`，完成后设置返回寄存器 R2

### 产物

```
output/kernel/group_token_old/
├── src/
│   └── group_token_old.o          ← 目标文件
└── elf/
    └── group_token_old.elf        ← 最终 ELF（PTO BlockISA 指令）
```

ELF 格式：`ELF 64-bit LSB executable, arch 0x105 (linx64v5)`，静态链接，bare-metal。

---

## ② 功能仿真：gfrun

### 执行命令

```bash
cd /mnt/workspace/gitCode/cann/Dev-experience/v300/SuperScalarModel
bin/gfrun -f /mnt/workspace/gitCode/cann/Dev-experience/v300/SuperNPUBench/benchmark/one-level-arch/output/kernel/group_token_old/elf/group_token_old.elf
```

### 原理

`gfrun` 是 SuperScalarModel 的功能模型（源码 `emulator/main.cpp`），逐块逐指令解释执行 BlockISA ELF，**不关心时序**，只验证计算结果正确性。

执行流程：
1. **加载 ELF**：解析段（`.text` / `.rodata` / `.bss` / `.data.rel.ro` / stack mem / map mem），映射到模拟内存
2. **从入口 PC 执行**：从 `.text` 段起始地址（如 `0x1126c`）开始执行
3. **逐 Block 解释**：解码 BlockISA 块指令，执行标量 / tile / SIMT 操作
4. **检查返回值**：`main()` 返回值写入 R2 寄存器，`R2 = 0` 表示 PASS

### 输出样式

```
Memory: 0x101c8 - 0x10210 (Size=0KB) [.rodata]       ← ELF 段加载信息
Memory: 0x1126c - 0x11583 (Size=0KB) [.text]
Memory: 0x125a0 - 0x80225a0 (Size=131136KB) [stack mem]
Memory: 0x4000802000 - 0x4008803000 (Size=131076KB) [map mem]
Starting from 0x1126c                                 ← 入口 PC

Thread:0Total Block number = 27154                    ← 执行的 block 数
Thread:0Total Inst number = 254075                    ← 执行的指令数

Total Block number = 27154
Total Inst number = 254075
Suaccelss to Reach the End of Benchmark! R2 = 0       ← R2=0 = PASS
```

**判定标准**：`R2 = 0` → PASS，`R2 = 1` → FAIL。

### 常用选项

```bash
bin/gfrun -f <elf> -X <start_pc_hex> -r <stop_pc_hex>   # 指定 PC 范围
bin/gfrun -f <elf> -c <max_blocks> -t 1                  # 限制 block 数 + trace
```

---

## ③ 时序仿真：gfsim

### 执行命令

```bash
cd /mnt/workspace/gitCode/cann/Dev-experience/v300/SuperScalarModel
bin/gfsim -f /mnt/workspace/gitCode/cann/Dev-experience/v300/SuperNPUBench/benchmark/one-level-arch/output/kernel/group_token_old/elf/group_token_old.elf
```

### 原理

`gfsim` 是 SuperScalarModel 的周期精确时序模型（源码 `TimingSim/core/main.cpp`），模拟完整的超标量 NPU 流水线：

1. **构建 SimSys**：搭建核心（BCC + Cube + Vector + TMA + LSU + L1/L2）、内存、调试、trace 模块
2. **加载 ELF**：同 gfrun
3. **逐周期推进**：遵循两阶段周期纪律
   - `cycle N: Work()` — 读取当前状态和输入队列
   - `cycle N: Xfer()` — 发布输出和下一状态
   - `cycle N+1: 下游 Work() 看到上一周期 Xfer() 写入的值`
4. **收集统计**：PMU 计数器、流水线占用率、停顿原因

### 输出样式

输出分多个统计段：

```
===================SuperScalar NPU Stats====================
Total Cycles.....................................:    258164    ← 总周期数
Sim Total Cycles.................................:    258164

================SuperScalar Unified Top-Down================   ← 流水线瓶颈分析
Retiring.........................................:     26.41%
Backend Bound....................................:     73.57%
  |-- Core Bound.................................:     71.35%
      |-- Scalar ALU.............................:    224.09%

===================superScalar Key Stats====================   ← 各引擎周期
superScalar Tileop Total Cycles..................:    258164
  |--Cube Tileop Total Cycles....................:         0
  |--Vector Tileop Total Cycles..................:         0
  |--TMA Tileop Total Cycles.....................:         0
superScalar Run Tileop Total Cycles..............:         0
  |--All Cores Idle Cycles.......................:    258164

====================BCC Key Stats====================        ← 控制核统计
Average Outstanding Block Number.................:      0.00

==================CubeCore 0 Statistics==================   ← Cube 核统计
Cube core alu utilization........................:         0    0.00%

===================(NODB) TMA 0 Key Stats===============    ← TMA 统计
TMA Tload........................................:         0

=====================Retired block Type=================    ← 退役 block 分类
Retired Block Num................................:     27154
  |--Retired STD Block Num.......................:     27154

======================Vector PMU Stats==================   ← Vector 引擎统计
TileopNum........................................:         0
```

### 关键指标解读

| 指标 | 含义 |
|---|---|
| `Total Cycles` | 总执行周期数（性能基准） |
| `Retiring %` | 有效退役指令占比（越高越好） |
| `Backend Bound %` | 后端阻塞占比（流水线瓶颈） |
| `Cube/Vector/TMA Tileop Total Cycles` | 各引擎活跃周期 |
| `All Cores Idle Cycles` | 所有引擎空闲周期 |
| `Retired Block Num` | 退役 block 数（对应执行的代码块） |
| `Scalar ALU %` | 标量 ALU 占用率 |

对于纯标量算子（如 group_token_old），Cube/Vector/TMA 统计全为 0，所有周期都是标量 ALU 在执行。

### 常用选项

```bash
bin/gfsim -f <elf> -t 1                           # trace 模式
bin/gfsim -f <elf> -s core.bp_mode=0              # 覆盖配置
bin/gfsim -f <elf> -p 1 --pipefile my_trace       # PipeView 可视化
bin/gfsim -f <elf> --swimlane 1 --swimfile my_swim # SwimLane 可视化
```

---

## 完整执行示例

以 `group_token_old` 算子为例的完整终端输出：

### 编译

```bash
$ export COMPILER_DIR=.../linx_blockisa_llvm_musl/bin
$ cd .../test/kernel/group_token_old
$ make TESTCASE=group_token_old PLAT=linx

clang++ -c -mlxbc -fenable-matrix -O2 ... -std=c++20 -D__linx \
  group_token_old.cpp -o group_token_old.o

clang++ -nostartfiles _start.s group_token_old.o -o group_token_old.elf
```

### 功能仿真

```bash
$ bin/gfrun -f .../group_token_old.elf

Memory: 0x1126c - 0x11583 (Size=0KB) [.text]
...
Starting from 0x1126c

Thread:0Total Block number = 27154
Thread:0Total Inst number = 254075

Total Block number = 27154
Total Inst number = 254075
Suaccelss to Reach the End of Benchmark! R2 = 0          ← PASS
```

### 时序仿真

```bash
$ bin/gfsim -f .../group_token_old.elf

...
Total Cycles.....................................:    258164
Sim Total Cycles.................................:    258164
superScalar Tileop Total Cycles..................:    258164
  |--All Cores Idle Cycles.......................:    258164
...
Retired Block Num................................:     27154
  |--Retired STD Block Num.......................:     27154
...
```

---

## 产物路径汇总

| 产物 | 绝对路径 |
|---|---|
| 目标文件 | `.../output/kernel/group_token_old/src/group_token_old.o` |
| ELF 文件 | `.../output/kernel/group_token_old/elf/group_token_old.elf` |
| gfrun 输出 | 终端标准输出（不落盘） |
| gfsim 输出 | 终端标准输出（不落盘） |
| PipeView（可选） | `<pipefile>.out`（需 `-p 1 --pipefile`） |
| SwimLane（可选） | `<swimfile>.json`（需 `--swimlane 1 --swimfile`） |

> gfrun/gfsim 默认不产生持久化文件，结果直接输出到终端。如需保存，可重定向：`bin/gfsim -f <elf> > result.log 2>&1`

---

## 工具链与仿真器的关系

```
linx-toolchain-build          SuperNPUBench              SuperScalarModel
   (编译器)                      (算子源码)                  (仿真器)
      │                             │                          │
      │ clang-15                    │ .cpp + .hpp              │ gfrun / gfsim
      │ -mlxbc                      │                          │
      │ target linx64v5             │ make PLAT=linx           │
      ▼                             ▼                          │
 linx_blockisa_llvm_musl  ──→  group_token_old.elf  ──→  gfrun (功能) / gfsim (时序)
      │                             │
      │ COMPILER_DIR                │ ELF path
      └───── export ────────────────┘
```

三个仓库同级，通过 `COMPILER_DIR` 环境变量和 ELF 文件路径串联。

---

## 具体执行命令

以下为可直接复制运行的完整命令，以 `group_token_old` 算子为例，使用绝对路径。

### 0. 环境准备（首次运行）

```bash
# 设置 Linx 工具链路径
export COMPILER_DIR=/mnt/workspace/gitCode/cann/Dev-experience/v300/linx-toolchain-build/output/linx_blockisa_llvm_musl/bin

# 验证编译器
$COMPILER_DIR/clang --version
# 期望输出: clang version 15.0.4, Target: linx64v5-unknown-linux-musl
```

### 1. 编译算子（源码 → ELF）

```bash
cd /mnt/workspace/gitCode/cann/Dev-experience/v300/SuperNPUBench/benchmark/one-level-arch/test/kernel/group_token_old

make TESTCASE=group_token_old PLAT=linx
```

验证产物：

```bash
ls -la /mnt/workspace/gitCode/cann/Dev-experience/v300/SuperNPUBench/benchmark/one-level-arch/output/kernel/group_token_old/elf/group_token_old.elf

file /mnt/workspace/gitCode/cann/Dev-experience/v300/SuperNPUBench/benchmark/one-level-arch/output/kernel/group_token_old/elf/group_token_old.elf
# 期望输出: ELF 64-bit LSB executable, *unknown arch 0x105*
```

### 2. 功能仿真（gfrun — 验证正确性）

```bash
cd /mnt/workspace/gitCode/cann/Dev-experience/v300/SuperScalarModel

bin/gfrun -f /mnt/workspace/gitCode/cann/Dev-experience/v300/SuperNPUBench/benchmark/one-level-arch/output/kernel/group_token_old/elf/group_token_old.elf
```

判定标准：输出末尾 `R2 = 0` 表示 PASS。

### 3. 时序仿真（gfsim — 测性能）

```bash
cd /mnt/workspace/gitCode/cann/Dev-experience/v300/SuperScalarModel

bin/gfsim -f /mnt/workspace/gitCode/cann/Dev-experience/v300/SuperNPUBench/benchmark/one-level-arch/output/kernel/group_token_old/elf/group_token_old.elf
```

关键输出：`Total Cycles: 258164`。

### 4. 一键全流程脚本

```bash
#!/bin/bash
set -e

export COMPILER_DIR=/mnt/workspace/gitCode/cann/Dev-experience/v300/linx-toolchain-build/output/linx_blockisa_llvm_musl/bin
BENCH=/mnt/workspace/gitCode/cann/Dev-experience/v300/SuperNPUBench
SIM=/mnt/workspace/gitCode/cann/Dev-experience/v300/SuperScalarModel
ELF=$BENCH/benchmark/one-level-arch/output/kernel/group_token_old/elf/group_token_old.elf

# 1. 编译
cd $BENCH/benchmark/one-level-arch/test/kernel/group_token_old
make TESTCASE=group_token_old PLAT=linx

# 2. 功能仿真
cd $SIM
bin/gfrun -f $ELF

# 3. 时序仿真
bin/gfsim -f $ELF
```

### 5. 其他常用命令

```bash
# 清理编译产物
cd /mnt/workspace/gitCode/cann/Dev-experience/v300/SuperNPUBench/benchmark/one-level-arch/test/kernel/group_token_old
make clean

# 生成反汇编
make TESTCASE=group_token_old PLAT=linx diss
# 产物: group_token_old.elf.diss

# 全量编译所有算子
cd /mnt/workspace/gitCode/cann/Dev-experience/v300/SuperNPUBench/benchmark/one-level-arch
bash compile_all.sh

# gfsim 带 PipeView 可视化
bin/gfsim -f $ELF -p 1 --pipefile /tmp/opencode/pipe_trace

# gfsim 带 SwimLane 可视化
bin/gfsim -f $ELF --swimlane 1 --swimfile /tmp/opencode/swim_trace

# gfsim 保存输出到文件
bin/gfsim -f $ELF > /tmp/opencode/gfsim_result.log 2>&1
```
