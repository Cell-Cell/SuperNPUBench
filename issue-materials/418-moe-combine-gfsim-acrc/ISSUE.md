# [gfsim] moe_combine 类 tile 密集算子触发参考模型误解码假 ACRC：Bad Syscall abort（a5dca25→d8903938 回归）

## 一、现象（一句话）

`moe_combine_v2` 算子 ELF（附材料包，预编译无需工具链）在 **gfrun 功能模型上两版全过**（R2=0，5472 inst / 1014 blocks），但在 **gfsim `d8903938` 的内嵌参考模型（RunReference 驱动的 SoftCore）中必崩**（`Bad Syscall Request: syscall(20e00, ...)`，exit 134）；**同一 ELF 在 `a5dca25` 上正常完成**（5205 cycles）——单变量隔离，模型侧回归。

- **非算子问题**：同一 ELF 双版 gfrun 全过；原始源码 ELF、双编译器产物 ELF 在 `a5dca25` gfsim 上也全过（下表 4 行 3 列全 PASS）；
- **非时序模型问题**：崩溃栈全部位于参考模型路径 `RunReference → EmulatorBlock → PostProcessBlock → ExecuteSysCall`；
- **最新 `687c37b`（08-29）仍未修复**：崩溃点变为显式断言 `Local TSTORE requires one legal source Tile descriptor`（`AccumulateBlockInfo.cpp:135`），并直接暴露**误解码**：`TPC:0x11762` 处真实指令为 `BSTART.TLSU TSTORE`，被解码为 `BSTART.TMA TMOV INVALID`。

## 二、版本信息

| 组件 | 分支 / Commit |
|---|---|
| SuperScalarModel（崩溃版） | `codex/pr-0.58.4-shared-model` @ **`d8903938`**（08-27 21:38） |
| SuperScalarModel（通过版） | `exp` @ **`a5dca25`**（08-24 构建） |
| SuperScalarModel（最新，仍未修复） | `687c37b`（08-29 00:08 "close v0.58.4 CUBE regression gaps"） |
| 算子 | `moe_combine_v2`（SuperNPUBench one-level-arch，源码见材料包 `kernels/`，来自 PTO-ISA/SuperNPUBench#74） |
| 编译器（对照） | TileOP-API `a795b97`/`f94bc12` × llvm `611105f2b`/`adcb87948` 双工具链交叉验证 |

## 三、复现步骤

```bash
git clone https://github.com/LinxISA/SuperScalarModel.git && cd SuperScalarModel
git checkout d8903938 && mkdir build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make gfsim gfrun -j
./bin/gfsim -f /path/to/moe_combine_v2_0828tool.elf   # 预期: Bad Syscall, exit 134
./bin/gfrun -t 1 -f /path/to/moe_combine_v2_0828tool.elf  # 对照: R2 = 0

git checkout a5dca25 && cd build && cmake .. && make gfsim gfrun -j
./bin/gfsim -f /path/to/moe_combine_v2_0828tool.elf   # 预期: 正常完成 5205 cycles
```

材料包 `repro.sh` 可一键跑完全部矩阵（clone + 双版本构建 + 5 枚 ELF）。

**gfsim 崩溃输出（关键行）：**

```
At emulator/SysCall.h 294 EcallAgent:
Bad Syscall Request: syscall(20e00, 0, 1d000, 1d000, 21000, 25000, 8);
FATAL: gfsim received signal 6
```

**gfrun 对照输出：**

```
Total Block number = 1014
Total Inst number = 5472
Suaccelss to Reach the End of Benchmark! R2 = 0
```

## 四、对照矩阵（单变量归因）

| ELF（源码 × 编译器） | a5dca25 gfsim | d8903938 gfsim | d8903938 gfrun |
|---|---|---|---|
| 修复版源码 × 0828 编译器（主复现） | **PASS** 5205 cyc | **CRASH** Bad Syscall(20e00) | PASS R2=0 |
| 修复版源码 × 0824 编译器 | **PASS** 5192 cyc | **CRASH** Bad Syscall | PASS R2=0 |
| 原始 TMOV/TCMP 源码 × 0824 编译器 | **PASS** 6038 cyc | **CRASH** Bad Syscall(80) | PASS R2=0 |
| 主复现 ELF × 687c37b（最新提交） | — | **CRASH** ASSERTION（见上） | CRASH 同断言 |

同一二进制，旧模型全过、新模型全崩；若是算子 bug，gfrun 与旧模型应当同崩。

## 五、梯度二分（排除纯体积因素）

对 kernel 做结构二分（材料包含全部变体 ELF）：

| 变体 | 总 blocks | d8903938 gfsim |
|---|---:|---|
| 空 kernel | 2 | PASS |
| 仅数据搬运路径 | 228 | PASS |
| 仅 flag 路径 | 196 | PASS |
| pack×1（完整 pack） | 291 | PASS |
| 仅 reduce 累加 | 355 | PASS |
| pack + reduce（完整算子） | 1014 | **CRASH** |
| **pack×2（代码逐字相同，仅调用两次）** | 618 | **CRASH** |
| 对照：moe_dispatch_v2（更多 blocks） | 11927 | PASS |

pack×1 过、pack×2 崩（代码零差异），且 dispatch 11927 blocks 也能过——**不是执行量阈值，是特定 tile 密集形状触发的参考模型累积状态腐蚀**。

## 六、根因分析（已定位到代码行，附日志）

### 6.1 崩溃机制

`TimingSim/core/main.cpp:359` 主循环每拍无条件调用 `sim->RunReference()`，用与 gfrun 相同的 SoftCore 做逐指令交叉验证。崩溃链：

```
SimSys::RunReference (TimingSim/infra/SimSys.cpp:61)
  → SoftCore::EmulatorBlock (emulator/SoftCore.cpp:209)
    → SoftCore::PostProcessBlock (emulator/SoftCore.cpp:488)
      → SoftCore::ExecuteSysCall (emulator/SysCall.cpp:8)
        → EcallAgent abort (emulator/SysCall.h:294)
```

`acrcReqType` 在 `emulator/engine/OperandIO.cpp:58` 由解码到 `OP_ACRC` 置位。但**整个 ELF 仅 `_end` 处有一条真 ACRC**（结束握手 `addi zero, 0x5e -> x1; acrc 1`），且程序远未执行到那里。崩溃时寄存器现场：X1=垃圾值（0x20e00）、A0-A5 仍残留算子参数指针（0x1d000=windowData、0x21000=windowFlag、0x25000=predBuf）——参考模型在算子 tile 代码深处**把流中字节误解码出一条假 ACRC**。

### 6.2 取指流腐蚀证据（TPC 轨迹错位）

`evidence/E7_tpc_tail_extract.txt`（`-t 3` 日志提取）：参考模型 TPC 序列出现 **4 字节指令中间 +2 错位**，如 `0x117c0 → 0x117c2 → 0x117c6`（真实指令边界为 0x117c0/0x117c4/0x117c8），即参考模型在 tile 指令序列附近**从指令中间开始解码**，随后把 tile 操作数字节当作标量指令流解码直至撞出假 ACRC。

### 6.3 `687c37b` 上的显式误解码证据

`evidence/E6_687c37b_gfsim_crash.log`（同 ELF × 最新提交 gfrun，可直接 `grep TMOV`）：

```
B393  T0 BPC 0x11762 [TMA] [FALL] [TMOV] BNext 0x0
M2561 |TPC:0x11762  ... BSTART.TMA TMOV INVALID |bin: 0xf8211181
gfrun: illegal instruction: ASSERTION FAILED: ... IsLegalLocalTileDescriptor ...
```

`0x11762` 处真实编码为 `BSTART.TLSU TSTORE FP32`（bin 0x08111181；被错误读成 0xf8211181——首字节已变），证实参考模型取指/解码流被腐蚀，而非 tile 语义实现问题。

### 6.4 疑点（供修复方向参考）

- `TimingSim/core/main.cpp:303`：`*(sim->refCore.memory) = sim->memory;` 参考模型内存由时序模型内存**拷贝构造**，两模型状态耦合；
- 参考模型按 `bctrl_bandwidth` 每拍被驱动多条 block（`SimSys.cpp:65-74`），与 gfrun 的自驱动节奏不同；
- 疑似参考模型在 tile 密集负载下的某处越界写/状态残留导致后续取指流错位（TPC +2 错位 + 指令字节改变均指向内存腐蚀），建议优先排查 TMA/TEPL 模拟器对 SoftMemory 的写路径。

## 七、材料包目录结构

```
.
├── ISSUE.md                                    # 本正文
├── README.md                                   # 材料导航
├── repro.sh                                    # 一键: clone + 双版本构建 + 5 ELF 矩阵
├── kernels/
│   ├── moe_combine_v2.hpp                      # 修复版源码（TSUB 替代 TMOV 等，见注释）
│   └── moe_combine_orig_tmov_tcmp.hpp          # 原始 TMOV/TCMP 版（证明与适配修改无关）
├── test/
│   ├── moe_combine_v2.cpp                      # 测试 main（含输出自校验）
│   └── _start.s                                # 裸机启动（结束 ACRC 握手）
├── elf/                                        # 5 枚预编译 ELF（见文件名后缀）
└── evidence/
    ├── E1_d8903938_gfsim_crash.log             # 主复现崩溃
    ├── E1_d8903938_gfrun_pass.log.gz           # 同 ELF gfrun R2=0
    ├── E2_a5dca25_gfsim_pass.log               # 同 ELF 旧模型 5205 cycles
    ├── E2_a5dca25_gfrun_pass.log.gz
    ├── E3_orig_d8903938_gfsim_crash.log        # 原始源码 ELF 同样崩
    ├── E4_pack1_d8903938_pass.log              # 梯度: pack×1 过
    ├── E5_pack2_d8903938_crash.log             # 梯度: pack×2 崩
    ├── E6_687c37b_gfsim_crash.log              # 最新提交: 显式误解码 TSTORE→TMOV INVALID
    ├── E7_tpc_tail_extract.txt                 # 参考模型 TPC +2 错位轨迹提取
    └── E7_crash_tail.txt
```

## 八、影响范围与关联

- 触发条件：tile 密集型 one-level 算子（每 block 多条 TLOAD/TSTORE/TEPL）在 gfsim 参考模型路径上累计执行一定量后取指流腐蚀；
- 已确认受影响算子：`moe_combine_v2`（SuperNPUBench one-level-arch）；`moe_dispatch_v2` 同期相似形状但未触发（11927 blocks 仍过），可用作对照定位腐蚀源；
- 算子来源 PR：https://github.com/PTO-ISA/SuperNPUBench/pull/74 （MC2 v300 验证算子集，含 moe_dispatch/moe_combine）
