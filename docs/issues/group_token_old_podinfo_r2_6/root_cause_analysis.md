# 附件 4：根因分析详细说明

## 一、问题陈述

`group_token_old` 算子在 gfrun 功能仿真器上运行，返回 `R2=6`，Phase 2 的 `tokenSuperPodInfo` 验证失败。其余 5 项验证全部通过。

## 二、关键证据：标准语义下验证全部通过

### 2.1 Python 数值验证

使用与 `genTopkIndex` 完全相同的 LCG 算法生成 `topkIndex`，在 Python 中模拟算子和验证逻辑的完整执行：

```python
# 算子用 >> 6 计算 pod
curDstPod = topkIndex[j] >> 6
dstPodLocal[curDstPod] = 1

# 验证用 / 64 计算 pod
curDstPod = expertId // 64
if curDstPod < kSuperPodNum:
    expectedPod[curDstPod] = 1
```

### 2.2 验证结果

| 指标 | 值 |
|---|---|
| `podInfoMatch` | **1024 / 1024** |
| 是否失配 | **False** |
| 返回值（标准语义） | **11000**（即 actualVal=1, expectedPod0=1, tokenId0=0） |

**结论**：在标准 C++/Python 语义下（`>> 6` 与 `/ 64` 等价），算子输出与验证期望**完全一致**，验证全部通过。

### 2.3 对比 gfrun 结果

| 环境 | 返回值 | 解码 | 验证结果 |
|---|---|---|---|
| Python（标准语义） | 11000 | actualVal=1, expectedPod0=1, tokenId0=0 | 1024/1024 PASS |
| **gfrun（PTO-ISA 仿真器）** | **6** | **actualVal=0, expectedPod0=0, tokenId0=6** | **FAIL** |

**关键发现**：
1. `tokenId0` 不同（Python=0, gfrun=6）：gfrun 中 scatter 顺序与标准语义不同（但 Phase 2 grouped ids 验证排序后比较，仍通过）
2. `actualVal` 不同（Python=1, gfrun=0）：算子写入 `tokenSuperPodInfo[0]` 的值在 gfrun 中为 0（标准语义下应为 1，因 token 6 有 pod 0 的 expert）
3. `expectedPod0` 不同（Python=1, gfrun=0）：验证逻辑计算的期望值在 gfrun 中为 0（标准语义下应为 1）

### 2.4 token 6 的 expert 分布（确定性数据）

`genTopkIndex` 使用 LCG 算法，输出是确定性的。token 6 的 16 个 top-k expert 为：

```
expert[ 0] =  57, pod(>>6) = 0, /64 = 0
expert[ 1] =  70, pod(>>6) = 1, /64 = 1
expert[ 2] = 126, pod(>>6) = 1, /64 = 1
expert[ 3] =  39, pod(>>6) = 0, /64 = 0
expert[ 4] =  57, pod(>>6) = 0, /64 = 0
expert[ 5] =  99, pod(>>6) = 1, /64 = 1
expert[ 6] =  47, pod(>>6) = 0, /64 = 0
expert[ 7] =  95, pod(>>6) = 1, /64 = 1
expert[ 8] =  14, pod(>>6) = 0, /64 = 0
expert[ 9] =  12, pod(>>6) = 0, /64 = 0
expert[10] = 116, pod(>>6) = 1, /64 = 1
expert[11] =  99, pod(>>6) = 1, /64 = 1
expert[12] =  93, pod(>>6) = 1, /64 = 1
expert[13] = 108, pod(>>6) = 1, /64 = 1
expert[14] = 112, pod(>>6) = 1, /64 = 1
expert[15] = 113, pod(>>6) = 1, /64 = 1
```

**标准语义下**：
- pod 0 有 expert（57, 39, 57, 47, 14, 12）→ `dstPodLocal[0] = 1`
- pod 1 有 expert（70, 126, 99, 95, 116, 99, 93, 108, 112, 113）→ `dstPodLocal[1] = 1`
- `tokenSuperPodInfo[0] = 1`（pod 0 标志），`tokenSuperPodInfo[1] = 1`（pod 1 标志）

**gfrun 中**：`actualVal = tokenSuperPodInfo[0] = 0`（应为 1），`expectedPod0 = 0`（应为 1）

## 三、根因分析

### 3.1 R2=6 的必要条件

```
ret = actualVal * 10000 + expectedPod0 * 1000 + tokenId0 = 6
```

要求 `actualVal=0, expectedPod0=0, tokenId0=6`（三者同时成立）。

根据 2.4 的 token 6 expert 分布，标准语义下 `actualVal` 应为 1、`expectedPod0` 应为 1。gfrun 中两者都为 0，说明：

1. **算子侧**（`>> 6`）：gfrun 中 `>> 6` 的结果不正确，导致 `dstPodLocal` 未被正确设置，`tokenSuperPodInfo` 写入 0
2. **验证侧**（`/ 64`）：gfrun 中 `/ 64` 的结果不正确，导致 `expectedPod` 始终为 0

### 3.2 最可能根因

**gfrun 功能仿真器对 `>>`（右移）和 `/`（整数除法）的语义处理存在缺陷**：

- 算子用 `>> 6` 计算 `curDstPod`，gfrun 未正确执行右移，`dstPodLocal` 未被正确设置 → `tokenSuperPodInfo` 写入 0
- 验证逻辑用 `/ kExpertPerPod` 计算 `curDstPod`，gfrun 未正确执行整数除法，`curDstPod` 返回错误值（≥ `kSuperPodNum`）→ `expectedPod` 始终为 0
- 两者在 token 6 位置恰好都为 0（`ret = 6`），但在其他位置产生不同的错误值 → `podInfoMatch != podInfoTotal`

### 3.3 支撑证据

#### 证据 1：标准语义下全部通过

Python 数值验证（2.2）证明在标准语义下算子与验证逻辑功能等价，验证 1024/1024 全部通过。gfrun 中失败纯粹是仿真器语义差异导致。

#### 证据 2：只有 pod info 验证使用 `>>` 或 `/`

| 验证项 | 是否使用 `>>` 或 `/` | 结果 |
|---|---|---|
| Phase 1 (expert counts) | 否（只用 `%` 和 `++`） | PASS |
| Phase 2 (section counts) | 否 | PASS |
| Phase 2 (grouped ids) | 否 | PASS |
| **Phase 2 (pod info)** | **是**（算子用 `>>`，验证用 `/`） | **FAIL** |
| Phase 3 (section bounds) | 否 | PASS |
| Phase 3 (sorted ids) | 否 | PASS |

**结论**：只有 pod info 验证涉及 `>>` 和 `/`，与根因假设完全吻合。

#### 证据 3：原报告推测被排除

原报告推测 podInfo 失败可能是 "bool/uint8_t 比较语义差异"，但修复 3（bool→uint32_t）后问题仍存在，排除此推测。

### 3.4 为什么 `tokenId0` 在 gfrun 中为 6 而非 0

Python 中 `groupedTokenIds[0] = 0`（token 0 先被 scatter 到 section 0），gfrun 中 `groupedTokenIds[0] = 6`。

这说明 gfrun 中 scatter 顺序与标准语义不同。可能原因：
- gfrun 中 `%`（取模）的语义也可能有细微差异，导致 `minLocalExpId` 计算不同
- 但 Phase 2 (grouped ids) 验证是排序后比较集合，不依赖顺序，仍通过

**注意**：`tokenId0 = 6` 不影响根因结论。无论 token 6 还是 token 0，标准语义下 `actualVal` 和 `expectedPod0` 都应为 1（两者都有 pod 0 的 expert），gfrun 中都为 0。

## 四、排除的其他候选

### 候选 2：写后清零模式的 store 重排

**描述**：算子中的写后清零模式可能导致 store buffer 重排：
```cpp
tokenSuperPodInfo[podInfoSectionOffset + j] = dstPodLocal[j];  // store A
dstPodLocal[j] = 0;                                             // store B
```

**分析**：
- 若 store B 影响了 store A 的值，`actualVal = 0`，但 `expectedPod0` 应为 1（标准语义）
- `ret = 0 + 1000 + 6 = 1006 ≠ 6`
- 无法解释 R2=6

**排除理由**：无法同时解释 `actualVal=0` 和 `expectedPod0=0`。

### 候选 3：bool 残留影响

**描述**：原报告推测 bool 类型问题。

**排除理由**：修复 3 已将 `dstPodLocal` 从 `bool[2]` 改为 `uint32_t[2]`，gfrun podInfo 失败仍存在。

### 候选 4：.bss 内存别名/覆盖

**描述**：`tokenSuperPodInfo` 所在 .bss 区域可能存在内存别名或覆盖。

**排除理由**：
- .bss 段大小 65KB（0x13f40-0x2438f），`tokenSuperPodInfo` 占 4KB，无溢出风险
- 若存在覆盖，其他验证项（如 groupedTokenIds）也应失败，但它们均通过
- Python 模拟中相同的内存布局验证全部通过

## 五、验证方法

为确认根因，建议以下验证方法（修改后重跑 gfrun）：

### 方法 A：最小用例测试 `>>` 和 `/`（推荐）

构造一个仅测试 gfrun 对 `>>` 和 `/` 语义的最小 ELF：

```cpp
int main() {
    uint32_t a = 100;
    uint32_t b_shift = a >> 6;    // 期望 1
    uint32_t b_div = a / 64;      // 期望 1
    // 返回 b_shift * 100 + b_div，期望 101
    return b_shift * 100 + b_div;
}
```

**预期**：若 `R2 != 101`，则确认 gfrun 对 `>>` 或 `/` 的语义处理有缺陷。

### 方法 B：算子与验证统一用 `>>`

将验证逻辑中所有 `/ kExpertPerPod` 改为 `>> 6`，同时保持算子的 `>> 6` 不变：

```cpp
// group_token_old.cpp 第 261 行和第 293 行
uint32_t curDstPod = expertId >> 6;   // 原为 expertId / kExpertPerPod
```

**预期**：若 `R2 = 0`（全部 PASS），说明 `>>` 在 gfrun 中正确，根因为 `/` 缺陷。若仍失败，说明 `>>` 也有缺陷。

### 方法 C：算子与验证统一用 `/`

将算子中所有 `>> 6` 改为 `/ kExpertPerPod`，同时保持验证的 `/ kExpertPerPod` 不变：

```cpp
// group_token_old.hpp 第 490 行
uint32_t curDstPod = topkIndex[j] / expertPerPod;   // 原为 topkIndex[j] >> 6
```

**预期**：若 `R2 = 0`，说明 `/` 在 gfrun 中正确，根因为 `>>` 缺陷。若仍失败，说明两者都有缺陷或根因在其他地方。

## 六、影响范围评估

### 6.1 对算子的影响

- 算子核心逻辑（Phase 1-3 的 histogram、scatter、sort）全部正确通过验证
- 仅 `tokenSuperPodInfo`（Phase 2 副产物）验证失败
- 不影响 token 分组和排序的正确性

### 6.2 对仿真器的影响

若根因确认为 gfrun 对 `>>` 和 `/` 的语义缺陷，可能影响：
- 其他在 gfrun 上使用右移 `>>` 或整数除法 `/` 的算子验证
- 需要全面排查 gfrun 的算术指令实现

### 6.3 临时规避方案

在根因修复前，可在算子和验证逻辑中统一使用 `>>` 或 `/`（而非混用），规避 gfrun 语义差异。但需先通过方法 A/B/C 确认哪个操作在 gfrun 中正确。

## 七、结论

**最可能根因**：gfrun 功能仿真器对 `>>`（右移）和 `/`（整数除法）的语义处理存在缺陷，导致算子侧 `>> 6` 和验证侧 `/ 64` 的计算结果均与标准语义不符，使 `tokenSuperPodInfo` 的实际值与期望值不匹配。

**关键证据**：Python 数值验证证明在标准语义下算子与验证逻辑功能等价（1024/1024 全部通过），gfrun 中失败纯粹是仿真器语义差异导致。

**置信度**：中高（标准语义下全部通过 + 只有使用 `>>`/`/` 的验证项失败 + R2=6 解码与假设一致）

**待确认**：需通过方法 A/B/C 之一实际验证，确定是 `>>`、`/` 还是两者都有缺陷。
