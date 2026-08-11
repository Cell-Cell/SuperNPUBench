# 附件 1：算子实现与验证逻辑的代码差异对照

## 一、差异总览

| 方面 | 算子实现（`groupToken_scalar`） | 验证逻辑（`main`） |
|---|---|---|
| 文件 | `kernels/group_token_old/group_token_old.hpp` | `test/kernel/group_token_old/src/group_token_old.cpp` |
| 函数 | `groupToken_scalar<true>`（第 462-506 行） | `main` 中 pod info 验证段（第 249-269 行） |
| pod 计算 | `topkIndex[j] >> 6`（右移） | `expertId / kExpertPerPod`（整数除法） |
| bounds check | **无** | **有**（`if (curDstPod < kSuperPodNum)`） |
| 数据来源 | 直接遍历 `topkIndex` | 从 `groupedTokenIds` 取 token id，再从 `topkIndex` 重算 |

## 二、算子实现（完整片段）

**文件**：`kernels/group_token_old/group_token_old.hpp:462-506`

```cpp
template <bool DoAtomicAdd>
static inline void groupToken_scalar(const uint32_t *topkIndex,
                                        uint32_t *groupedTokenIds,
                                        uint32_t *tokenSuperPodInfo,
                                        uint32_t *expertSectionTokenCnt,
                                        uint32_t batchSize,
                                        uint32_t topk,
                                        uint32_t expertPerRank,
                                        uint32_t expertPerPod,
                                        uint32_t superPodNum)
{
    for (uint32_t i = 0; i < expertPerRank; i++) {
        expertSectionTokenCnt[i] = 0;
    }

    uint32_t dstPodLocal[kSuperPodNum];
    for (uint32_t i = 0; i < superPodNum; i++) {
        dstPodLocal[i] = 0;
    }

    for (uint32_t i = 0; i < batchSize; i++) {
        uint32_t minLocalExpId = expertPerRank;
        uint32_t stop = (i + 1) * topk;
        for (uint32_t j = i * topk; j < stop; j++) {
            uint32_t curLocalExpId = topkIndex[j] % expertPerRank;
            if (curLocalExpId < minLocalExpId) {
                minLocalExpId = curLocalExpId;
            }
            uint32_t curDstPod = topkIndex[j] >> 6;        // ← 右移 6 位
            dstPodLocal[curDstPod] = 1;                      // ← 无 bounds check
        }
        uint32_t idxInSection;
        if constexpr (DoAtomicAdd) {
            idxInSection = expertSectionTokenCnt[minLocalExpId]++;
        } else {
            idxInSection = expertSectionTokenCnt[minLocalExpId] + 1;
        }
        groupedTokenIds[minLocalExpId * batchSize + idxInSection] = i;
        uint32_t podInfoSectionOffset = minLocalExpId * batchSize * superPodNum + idxInSection * superPodNum;
        for (uint32_t j = 0; j < superPodNum; j++) {
            tokenSuperPodInfo[podInfoSectionOffset + j] = dstPodLocal[j];   // ← 写入 pod info
            dstPodLocal[j] = 0;                                              // ← 写后清零
        }
    }
}
```

### 关键点

1. **第 490 行**：`curDstPod = topkIndex[j] >> 6` — 使用右移计算目标 pod
2. **第 491 行**：`dstPodLocal[curDstPod] = 1` — 无 bounds check（依赖 expertId ∈ [0,127] 保证 curDstPod ∈ {0,1}）
3. **第 502-503 行**：写入 `tokenSuperPodInfo` 后立即清零 `dstPodLocal[j]`

## 三、验证逻辑（完整片段）

**文件**：`test/kernel/group_token_old/src/group_token_old.cpp:249-269`

```cpp
// --- verify Phase 2: tokenSuperPodInfo (paired with groupedTokenIds) ---
int podInfoMatch = 0;
int podInfoTotal = 0;
for (uint32_t s = 0; s < kExpertPerRank; s++) {
    uint32_t n = refSectionCnt[s];
    podInfoTotal += n * kSuperPodNum;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t tokenId = groupedTokenIds[s * kBS + i];
        uint32_t expectedPod[kSuperPodNum];
        for (uint32_t j = 0; j < kSuperPodNum; j++) expectedPod[j] = 0;
        for (uint32_t j = 0; j < kTopK; j++) {
            uint32_t expertId = topkIndex[tokenId * kTopK + j];
            uint32_t curDstPod = expertId / kExpertPerPod;    // ← 整数除法 / 64
            if (curDstPod < kSuperPodNum)                     // ← 有 bounds check
                expectedPod[curDstPod] = 1;
        }
        uint32_t offset = s * kBS * kSuperPodNum + i * kSuperPodNum;
        for (uint32_t j = 0; j < kSuperPodNum; j++) {
            if (tokenSuperPodInfo[offset + j] == expectedPod[j]) podInfoMatch++;
        }
    }
}
```

### 关键点

1. **第 261 行**：`curDstPod = expertId / kExpertPerPod` — 使用整数除法计算目标 pod
2. **第 262 行**：`if (curDstPod < kSuperPodNum)` — 有 bounds check
3. **第 266 行**：逐元素比较 `tokenSuperPodInfo[offset + j] == expectedPod[j]`

## 四、返回值编码逻辑

**文件**：`test/kernel/group_token_old/src/group_token_old.cpp:283-301`

```cpp
int ret = 0;
if (cntMatch != (int)kExpertNum) ret = 1;
else if (secMatch != (int)kExpertPerRank) ret = 2;
else if (idMatch != idTotal) ret = 3;
else if (podInfoMatch != podInfoTotal) {
    uint32_t s0 = 0, i0 = 0;
    uint32_t tokenId0 = groupedTokenIds[s0 * kBS + i0];
    uint32_t expectedPod0 = 0;
    for (uint32_t j = 0; j < kTopK; j++) {
        uint32_t expertId = topkIndex[tokenId0 * kTopK + j];
        uint32_t curDstPod = expertId / kExpertPerPod;        // ← 整数除法
        if (curDstPod < kSuperPodNum) expectedPod0 = 1;
    }
    uint32_t offset0 = s0 * kBS * kSuperPodNum + i0 * kSuperPodNum;
    uint32_t actualVal = tokenSuperPodInfo[offset0];
    ret = actualVal * 10000 + expectedPod0 * 1000 + tokenId0;
}
else if (boundMatch != (int)(kExpertPerRank + 1)) ret = 4;
else if (sortMatch != (int)kBS) ret = 5;
```

### 编码格式

```
ret = actualVal * 10000 + expectedPod0 * 1000 + tokenId0
       ^                 ^                  ^
       算子实际写入值     验证逻辑期望值      首个失配 token id
```

## 五、差异影响分析

### 5.1 语义等价性

对无符号整数，`x >> 6` 与 `x / 64` 在**标准 C++ 语义下完全等价**（因 64 = 2^6，且无符号右移为逻辑右移）。

**Python 数值验证证明**：使用相同的 `topkIndex` 数据，算子（`>> 6`）和验证（`/ 64`）在标准语义下输出完全一致，验证 1024/1024 全部通过。

### 5.2 在 gfrun 中的行为

gfrun 返回 R2=6（验证失败），而标准语义下应返回 11000（全部通过）。这说明 `>> 6` 和 `/ 64` 在 gfrun 中**均未按标准语义执行**：

- **算子侧**（用 `>>`）：gfrun 中右移结果不正确 → `dstPodLocal` 未被正确设置 → `tokenSuperPodInfo` 写入 0
- **验证侧**（用 `/`）：gfrun 中除法结果不正确 → `curDstPod` 返回错误值（≥ `kSuperPodNum`）→ `expectedPod` 始终为 0

### 5.3 为什么只有 pod info 验证失败

| 验证项 | 是否使用 `>>` 或 `/` | 结果 |
|---|---|---|
| Phase 1 (expert counts) | 否（只用 `%` 和 `++`） | PASS |
| Phase 2 (section counts) | 否 | PASS |
| Phase 2 (grouped ids) | 否 | PASS |
| **Phase 2 (pod info)** | **是**（算子用 `>>`，验证用 `/`） | **FAIL** |
| Phase 3 (section bounds) | 否 | PASS |
| Phase 3 (sorted ids) | 否 | PASS |

**结论**：只有 pod info 验证使用了 `>>` 或 `/`，与"gfrun 对 `>>` 和 `/` 均有缺陷"的根因假设完全吻合。
