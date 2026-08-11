# [Bug] group_token_old 算子 Phase 2 tokenSuperPodInfo 验证在 gfrun 功能仿真器中失败（R2=6）

## 一、问题概述

| 项目 | 内容 |
|---|---|
| **算子** | `group_token_old`（MoE Dispatch 三阶段实现，标量路径） |
| **仿真器** | `SuperScalarModel/bin/gfrun`（PTO-ISA 功能仿真器） |
| **编译器** | `linx-toolchain-build`（clang-15，target `linx64v5`） |
| **现象** | gfrun 运行 ELF 后返回 `R2 = 6`，Phase 2 的 `tokenSuperPodInfo` 验证不匹配 |
| **严重程度** | 中（核心逻辑 5/6 项通过，仅 pod info 副产物验证失败；不阻塞算子主功能） |
| **状态** | 根因已定位为候选方向，需确认 |

### 简述

在 PTO-ISA 功能仿真器 `gfrun` 上运行 `group_token_old` 算子 ELF，6 项验证中 **5 项 PASS、1 项 FAIL**。失败项为 Phase 2 的 `tokenSuperPodInfo`（pod info）验证，返回码 `R2=6`。

经过代码审查、返回值编码反推和 Python 数值验证，**最可能根因**是：gfrun 功能仿真器对 `>>`（右移）和 `/`（整数除法）的语义处理存在缺陷。Python 验证证明在标准语义下算子与验证逻辑功能等价（1024/1024 全部通过），gfrun 中失败纯粹是仿真器语义差异导致。算子用 `>> 6`、验证用 `/ 64`，两者在 gfrun 中均产生错误结果，使 `tokenSuperPodInfo` 的实际值与期望值不匹配。

---

## 二、复现环境

| 项目 | 值 |
|---|---|
| 仓库 | `SuperNPUBench` + `SuperScalarModel` + `linx-toolchain-build` |
| 编译器路径 | `/mnt/workspace/gitCode/cann/Dev-experience/v300/linx-toolchain-build/output/linx_blockisa_llvm_musl/bin/clang++` |
| 编译器版本 | clang 15.0.4, Target: `linx64v5-unknown-linux-musl` |
| 仿真器路径 | `/mnt/workspace/gitCode/cann/Dev-experience/v300/SuperScalarModel/bin/gfrun` |
| 算子源码 | `SuperNPUBench/benchmark/one-level-arch/kernels/group_token_old/group_token_old.hpp` |
| 测试驱动 | `SuperNPUBench/benchmark/one-level-arch/test/kernel/group_token_old/src/group_token_old.cpp` |
| 执行日期 | 2026-08-10 |

### MoE 拓扑参数

| 参数 | 值 | 说明 |
|---|---|---|
| `kBS` | 512 | batch size（token 总数） |
| `kTopK` | 16 | 每个 token 选中的 top-k expert 数 |
| `kExpertPerRank` | 4 | 每个 rank 的本地 expert 数 |
| `kExpertPerPod` | 64 | 每个 pod 的 expert 数 |
| `kSuperPodNum` | 2 | super pod 数量 |
| `kExpertNum` | 128 | 全局 expert 总数 |

---

## 三、复现步骤

```bash
#!/bin/bash
set -e

export COMPILER_DIR=/mnt/workspace/gitCode/cann/Dev-experience/v300/linx-toolchain-build/output/linx_blockisa_llvm_musl/bin
BENCH=/mnt/workspace/gitCode/cann/Dev-experience/v300/SuperNPUBench
SIM=/mnt/workspace/gitCode/cann/Dev-experience/v300/SuperScalarModel
ELF=$BENCH/benchmark/one-level-arch/output/kernel/group_token_old/elf/group_token_old.elf

# 1. 编译算子（源码 → ELF）
cd $BENCH/benchmark/one-level-arch/test/kernel/group_token_old
make TESTCASE=group_token_old PLAT=linx

# 2. 功能仿真（验证正确性）
cd $SIM
bin/gfrun -f $ELF
```

### 预期输出

```
Memory: 0x10200 - 0x10248 (Size=0KB) [.rodata]
...
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

**判定标准**：`R2 = 0` → 全部 PASS；`R2 = 6` → podInfo 验证失败（其他 5 项通过）。

---

## 四、验证结果明细

| 验证项 | 错误码 | 结果 | 说明 |
|---|---|---|---|
| Phase 1 (expert counts) | R2=1 | **PASS** | 128/128 expert 计数匹配 |
| Phase 2 (section counts) | R2=2 | **PASS** | 4/4 分区计数匹配 |
| Phase 2 (grouped ids) | R2=3 | **PASS** | 全部分组 token id 匹配（排序后比较） |
| **Phase 2 (pod info)** | **R2=6** | **FAIL** | **tokenSuperPodInfo 验证不匹配** |
| Phase 3 (section bounds) | R2=4 | **PASS** | 5/5 分区边界匹配 |
| Phase 3 (sorted ids) | R2=5 | **PASS** | 512/512 排序 token id 精确匹配 |

> 完整仿真报告见附件 `evidence_sim_report_excerpt.md`。

---

## 五、根因分析

### 5.1 算子与验证逻辑的关键差异

| 方面 | 算子实现（`groupToken_scalar`） | 验证逻辑（`main`） |
|---|---|---|
| pod 计算 | `topkIndex[j] >> 6`（**右移**） | `expertId / kExpertPerPod`（**整数除法**） |
| bounds check | **无** | **有**（`if (curDstPod < kSuperPodNum)`） |
| 数据来源 | 直接从 `topkIndex` 读取 | 从 `groupedTokenIds` 取 token id，再从 `topkIndex` 重算 |

**算子实现**（`group_token_old.hpp:485-491`）：
```cpp
for (uint32_t j = i * topk; j < stop; j++) {
    uint32_t curLocalExpId = topkIndex[j] % expertPerRank;
    if (curLocalExpId < minLocalExpId) {
        minLocalExpId = curLocalExpId;
    }
    uint32_t curDstPod = topkIndex[j] >> 6;      // 右移 6 位 = 除以 64
    dstPodLocal[curDstPod] = 1;                   // 无 bounds check
}
```

**验证逻辑**（`group_token_old.cpp:259-263`）：
```cpp
for (uint32_t j = 0; j < kTopK; j++) {
    uint32_t expertId = topkIndex[tokenId * kTopK + j];
    uint32_t curDstPod = expertId / kExpertPerPod;  // 整数除法 / 64
    if (curDstPod < kSuperPodNum)                   // 有 bounds check
        expectedPod[curDstPod] = 1;
}
```

### 5.2 R2=6 返回值解码

验证失败时，返回值编码逻辑（`group_token_old.cpp:287-299`）：

```cpp
else if (podInfoMatch != podInfoTotal) {
    uint32_t s0 = 0, i0 = 0;                                    // 第 0 个分区、第 0 个位置
    uint32_t tokenId0 = groupedTokenIds[0];                      // 该位置的 token id
    uint32_t expectedPod0 = 0;
    for (uint32_t j = 0; j < kTopK; j++) {
        uint32_t expertId = topkIndex[tokenId0 * kTopK + j];
        uint32_t curDstPod = expertId / kExpertPerPod;
        if (curDstPod < kSuperPodNum) expectedPod0 = 1;
    }
    uint32_t actualVal = tokenSuperPodInfo[0];                   // 算子实际写入的值
    ret = actualVal * 10000 + expectedPod0 * 1000 + tokenId0;
}
```

反推 R2=6：

- `genTopkIndex` 生成的 `expertId ∈ [0, 127]`，`curDstPod = expertId / 64 ∈ {0, 1}`，均 `< kSuperPodNum(2)`
- 因此 **`expectedPod0` 在正确语义下恒为 1**
- 此时 `ret = actualVal * 10000 + 1000 + tokenId0`，最小值为 1000，**不可能等于 6**

R2=6 成立的唯一条件：

| 变量 | 值 | 含义 |
|---|---|---|
| `actualVal` | **0** | 算子写入 `tokenSuperPodInfo[0]` 的值为 0 |
| `expectedPod0` | **0** | 验证逻辑计算的期望值也为 0（**应为 1**） |
| `tokenId0` | **6** | 第 0 个分区的第 0 个 token 是 token id 6 |

`ret = 0 * 10000 + 0 * 1000 + 6 = 6` ✓

> 完整解码分析见附件 `evidence_r2_decode.md`。

### 5.3 最可能根因

**gfrun 功能仿真器对 `>>`（右移）和 `/`（整数除法）的语义处理存在缺陷**：

**关键证据**：使用与 `genTopkIndex` 完全相同的 LCG 算法在 Python 中模拟算子和验证逻辑的完整执行，在标准语义下（`>> 6` 与 `/ 64` 等价），验证 **1024/1024 全部通过**，返回值应为 11000。gfrun 返回 R2=6，纯粹是仿真器语义差异导致。

| 环境 | 返回值 | 解码 | 验证结果 |
|---|---|---|---|
| Python（标准语义） | 11000 | actualVal=1, expectedPod0=1, tokenId0=0 | 1024/1024 PASS |
| **gfrun（PTO-ISA 仿真器）** | **6** | **actualVal=0, expectedPod0=0, tokenId0=6** | **FAIL** |

- 算子用 `>> 6` 计算 `curDstPod`，gfrun 中 `>>` 结果不正确 → `dstPodLocal` 未被正确设置 → `tokenSuperPodInfo` 写入 0
- 验证逻辑用 `/ kExpertPerPod` 计算 `curDstPod`，gfrun 中 `/` 结果不正确 → `expectedPod` 始终为 0
- 两者在 token 6 位置恰好都为 0（`ret = 6`），但在其他位置产生不同的错误值 → `podInfoMatch != podInfoTotal`

### 5.4 支撑证据

**为什么其余 5 项验证通过**：只有 pod info 验证使用了 `>>` 或 `/`，其余 5 项均不涉及这些运算：

| 验证项 | 为何通过 |
|---|---|
| Phase 1 (expert counts) | 只用 `%`（取模）和 `++`，不涉及 `>>` 或 `/` |
| Phase 2 (section counts) | 只比较 `expertSectionTokenCnt`，不涉及 pod 计算 |
| Phase 2 (grouped ids) | 只比较 token id，不涉及 pod info |
| Phase 3 (section bounds) | 只比较 `sectionStarts`，不涉及 pod 计算 |
| Phase 3 (sorted ids) | 只比较 `sortedTokenIds`，不涉及 pod 计算 |

> 完整根因分析与代码对照见附件 `root_cause_analysis.md` 和 `evidence_code_diff.md`。

---

## 六、已排查的相关问题

报告中已修复 3 个编译/仿真问题，均与 podInfo 失败**无直接因果关系**：

| 修复 | 问题 | 与 podInfo 关系 |
|---|---|---|
| 修复1 | 移除 `topkIndexLocal[8192]` 冗余栈数组（32KB 栈溢出导致死循环） | 无关（已修复，podInfo 仍失败） |
| 修复2 | 大数组改为 `static` 移入 .bss（~77KB 超出 ~65KB 栈帧） | 无关（已修复，podInfo 仍失败） |
| 修复3 | `dstPodLocal` 从 `bool[2]` 改为 `uint32_t[2]`（gfsim `sb` 指令崩溃） | 修复了 gfsim 崩溃，但 gfrun podInfo 失败仍存在 |

> 完整修复记录见附件 `evidence_sim_report_excerpt.md`。

---

## 七、建议的验证方法

为确认根因，建议以下验证方法（修改后重跑 gfrun）：

### 方法 A：最小用例测试 `>>` 和 `/`（推荐）

构造一个仅测试 gfrun 对 `>>` 和 `/` 语义的最小 ELF：

```cpp
int main() {
    uint32_t a = 100;
    uint32_t b_shift = a >> 6;    // 期望 1
    uint32_t b_div = a / 64;      // 期望 1
    return b_shift * 100 + b_div; // 期望 101
}
```

**预期**：若 `R2 != 101`，则确认 gfrun 对 `>>` 或 `/` 的语义处理有缺陷。

### 方法 B：算子与验证统一用 `>>`

将验证逻辑中所有 `/ kExpertPerPod` 改为 `>> 6`，保持算子的 `>> 6` 不变。若 `R2 = 0`，说明 `>>` 正确，根因为 `/` 缺陷。

### 方法 C：算子与验证统一用 `/`

将算子中所有 `>> 6` 改为 `/ kExpertPerPod`，保持验证的 `/ kExpertPerPod` 不变。若 `R2 = 0`，说明 `/` 正确，根因为 `>>` 缺陷。

> 完整验证脚本见附件 `reproduce_steps.md`。

---

## 八、影响范围

- 算子核心逻辑（Phase 1-3 的 histogram、scatter、sort）全部正确通过验证
- 仅 `tokenSuperPodInfo`（Phase 2 副产物，记录 token 的目标 pod 调度元数据）验证失败
- 不影响 token 分组和排序的正确性
- 若根因为 gfrun 除法缺陷，可能影响其他在 gfrun 上使用整数除法的算子验证

---

## 九、附件清单

| 附件 | 说明 |
|---|---|
| `evidence_code_diff.md` | 算子实现与验证逻辑的代码差异对照 |
| `evidence_r2_decode.md` | R2=6 返回值解码分析 |
| `evidence_sim_report_excerpt.md` | 仿真报告关键摘要（含修复记录、验证结果、ELF 段映射） |
| `root_cause_analysis.md` | 根因分析详细说明 |
| `reproduce_steps.md` | 一键复现脚本 |

---

## 十、相关链接

- 仿真报告：`SuperNPUBench/docs/workflow/group_token_old_sim_report.md`
- 算子源码：`SuperNPUBench/benchmark/one-level-arch/kernels/group_token_old/group_token_old.hpp`
- 测试驱动：`SuperNPUBench/benchmark/one-level-arch/test/kernel/group_token_old/src/group_token_old.cpp`
- 功能仿真器：`SuperScalarModel/bin/gfrun`
