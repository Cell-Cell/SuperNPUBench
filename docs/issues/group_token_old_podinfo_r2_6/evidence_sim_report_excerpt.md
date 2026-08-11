# 附件 3：仿真报告关键摘要

> 源文件：`SuperNPUBench/docs/workflow/group_token_old_sim_report.md`
> 采集日期：2026-08-10

## 一、算子概述

`group_token_old` 是 MoE Dispatch 的完整三阶段实现（含 SIMD 排序的 "old" 变体）：

| 阶段 | 功能 |
|---|---|
| Phase 1: CalTokenPerExpertCnt | 统计每个 expert 收到的 token 数（直方图） |
| Phase 2: GroupToken | 把 token 按"本地 expert" scatter 分组，记录 tokenSuperPodInfo |
| Phase 3: SortKernel | FloorFunc 计算 per-token minLocalExpId，再按 minLocalExpId 排序，生成连续分区 token id 列表 |

当前实现为纯标量路径（`__linx` 宏下走 scalar 分支，不触发 SIMT/SIMD 加速路径）。

## 二、源码修复记录

编译仿真过程中发现源码存在 3 个问题，已修复：

### 修复 1：移除 `topkIndexLocal[8192]` 冗余栈数组

- **文件**：`kernels/group_token_old/group_token_old.hpp`
- **问题**：`groupToken_scalar` 函数中声明了 `uint32_t topkIndexLocal[kTopKEleNum]`（32KB 本地数组），这只是 `topkIndex` 参数的冗余拷贝。在 PTO-ISA 仿真器中，该 32KB 栈分配导致栈帧溢出，破坏了循环计数器 `s8`（从 8192 变成 0xFF196A ≈ 16M），使 gfrun 实质死循环。
- **修复**：移除 `topkIndexLocal` 数组及其拷贝循环，直接使用 `topkIndex` 参数。
- **与 podInfo 关系**：无关（已修复，podInfo 失败仍存在）

### 修复 2：大数组改为 `static` 移入 .bss

- **文件**：`test/kernel/group_token_old/src/group_token_old.cpp`、`kernels/group_token_old/group_token_old.hpp`
- **问题**：`main()` 中大量本地数组（`topkIndex[8192]`=32KB、`groupedTokenIds[2048]`=8KB、`tokenSuperPodInfo[4096]`=4KB、引用数组 ~11KB、验证用 `buf[512]`/`ref[512]`=4KB、`minLocalExpIds[512]`=2KB）总计约 77KB，超过了编译器分配的栈帧约 65KB，导致栈覆盖和循环计数器损坏。
- **修复**：将 `main()` 和内联函数中的大数组声明改为 `static`，移入 `.bss` 段，消除栈帧溢出。`.bss` 段从 0KB 扩展到 65KB。
- **与 podInfo 关系**：无关（已修复，podInfo 失败仍存在）

### 修复 3：`dstPodLocal` 从 `bool[2]` 改为 `uint32_t[2]`

- **文件**：`kernels/group_token_old/group_token_old.hpp`
- **问题**：`groupToken_scalar` 中 `bool dstPodLocal[2]` 编译后生成 `sb`（字节写）指令，在 gfsim 时序仿真器的紧密循环中触发 "Unrecognized branch resolve" 断言崩溃（cycle 243786, BPC 0x114c0）。
- **修复**：将 `dstPodLocal` 类型从 `bool[2]` 改为 `uint32_t[2]`，编译器改用 `sw`（字写）指令，gfsim 不再崩溃。
- **与 podInfo 关系**：修复了 gfsim 崩溃，但 **gfrun podInfo 验证失败仍然存在**

## 三、功能仿真结果（gfrun）

**结论：5/6 验证项 PASS，podInfo 验证为已知问题（R2 = 6）**

### 完整输出

```
Memory: 0x10200 - 0x10248 (Size=0KB) [.rodata]
Memory: 0x1024c - 0x1025f (Size=0KB) [.eh_frame_hdr]
Memory: 0x10260 - 0x102ab (Size=0KB) [.eh_frame]
Memory: 0x112ac - 0x11f27 (Size=3KB) [.text]
Memory: 0x12f28 - 0x12f3f (Size=0KB) [.data.rel.ro]
Memory: 0x13f40 - 0x2438f (Size=65KB) [.bss]
Memory: 0x24390 - 0x8034390 (Size=131136KB) [stack mem]
Memory: 0x4000802000 - 0x4008803000 (Size=131076KB) [map mem]
Starting from 0x112ac

Thread:0Total Block number = 467238
Thread:0Total Inst number = 2963498

Total Block number = 467238
Total Inst number = 2963498
Suaccelss to Reach the End of Benchmark! R2 = 6
```

### 验证项明细

| 验证项 | 错误码 | 结果 | 说明 |
|---|---|---|---|
| Phase 1 (expert counts) | R2=1 | **PASS** | 128/128 expert 计数匹配 |
| Phase 2 (section counts) | R2=2 | **PASS** | 4/4 分区计数匹配 |
| Phase 2 (grouped ids) | R2=3 | **PASS** | 全部分组 token id 匹配（排序后比较） |
| Phase 2 (pod info) | R2=6 | **FAIL** | tokenSuperPodInfo 验证不匹配 |
| Phase 3 (section bounds) | R2=4 | **PASS** | 5/5 分区边界匹配 |
| Phase 3 (sorted ids) | R2=5 | **PASS** | 512/512 排序 token id 精确匹配 |

### 关键指标

| 指标 | 值 | 说明 |
|---|---|---|
| 入口 PC | `0x112ac` | `.text` 段起始地址 |
| Total Block number | 467,238 | 执行的 block 总数 |
| Total Inst number | 2,963,498 | 执行的指令总数 |
| **R2 返回值** | **6** | podInfo 验证失败（5/6 项通过） |

## 四、ELF 段映射

| 段名 | 起始地址 | 结束地址 | 大小 |
|---|---|---|---|
| `.rodata` | `0x10200` | `0x10248` | 0 KB |
| `.eh_frame_hdr` | `0x1024c` | `0x1025f` | 0 KB |
| `.eh_frame` | `0x10260` | `0x102ab` | 0 KB |
| `.text` | `0x112ac` | `0x11f27` | 3 KB |
| `.data.rel.ro` | `0x12f28` | `0x12f3f` | 0 KB |
| `.bss` | `0x13f40` | `0x2438f` | 65 KB |
| `stack mem` | `0x24390` | `0x8034390` | 131,136 KB |
| `map mem` | `0x4000802000` | `0x4008803000` | 131,076 KB |

## 五、性能结果摘要（gfsim 时序仿真）

| 指标 | 值 |
|---|---|
| **Total Cycles** | **258,430** |
| Simulation time | 32s |
| Retired Block Num | 27,160 |
| BPC (Blocks Per Cycle) | 0.11 |
| Retiring | 26.51% |
| Backend Bound | 73.46% |
| 主要瓶颈 | 标量 ALU 严重过载（217.55%） |

## 六、已知问题汇总（原文）

1. **podInfo 验证失败（R2=6）**：`tokenSuperPodInfo` 验证不匹配，可能是 PTO-ISA 仿真器对 `bool`/`uint8_t` 在紧密循环中的比较语义处理有差异。不影响算子核心逻辑（Phase 1-3 的 histogram、scatter、sort 均正确通过验证）。
2. **gfsim 验证段 O(n²) 排序性能问题**：验证段含冒泡排序（O(n²)），在 gfsim 中因分支误预测导致 >23M cycles（运行 >60 分钟未完成）。建议性能采集时跳过验证段。
3. **`bool` 类型在 gfsim 中的 `sb` 指令问题**：`bool` 数组在紧密循环中生成 `sb`（字节写）指令，导致 gfsim 分支预测异常崩溃。已通过将 `dstPodLocal` 改为 `uint32_t` 修复。

> **注**：原报告对 podInfo 失败的推测（bool/uint8_t 比较语义差异）在修复 3（bool→uint32_t）后仍未解决，本 Issue 重新分析后认为根因为 gfrun 整数除法缺陷，详见 `root_cause_analysis.md`。
