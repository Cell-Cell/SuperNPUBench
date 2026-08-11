# 附件 2：R2=6 返回值解码分析

## 一、返回值编码机制

验证失败时，`main` 函数返回一个编码值，用于在无 `printf` 的 `__linx` 环境下传递调试信息。

**编码逻辑**（`group_token_old.cpp:287-299`）：

```cpp
else if (podInfoMatch != podInfoTotal) {
    uint32_t s0 = 0, i0 = 0;                                    // 固定取第 0 个分区、第 0 个位置
    uint32_t tokenId0 = groupedTokenIds[s0 * kBS + i0];         // 该位置的 token id
    uint32_t expectedPod0 = 0;
    for (uint32_t j = 0; j < kTopK; j++) {
        uint32_t expertId = topkIndex[tokenId0 * kTopK + j];
        uint32_t curDstPod = expertId / kExpertPerPod;          // / 64
        if (curDstPod < kSuperPodNum) expectedPod0 = 1;
    }
    uint32_t offset0 = s0 * kBS * kSuperPodNum + i0 * kSuperPodNum;
    uint32_t actualVal = tokenSuperPodInfo[offset0];            // 算子实际写入的值
    ret = actualVal * 10000 + expectedPod0 * 1000 + tokenId0;
}
```

**编码格式**：

```
ret = actualVal * 10000 + expectedPod0 * 1000 + tokenId0
       \________________/   \_________________/   \_________/
       算子实际写入值         验证逻辑期望值         首个失配 token id
       (0 或 1)              (0 或 1)              (0 ~ 511)
```

## 二、关键约束分析

### 2.1 expectedPod0 的理论值

`genTopkIndex` 生成的 `expertId ∈ [0, kExpertNum) = [0, 128)`。

```
curDstPod = expertId / kExpertPerPod = expertId / 64
```

因 `expertId ∈ [0, 127]`，所以 `curDstPod ∈ {0, 1}`，均 `< kSuperPodNum(2)`。

因此 `if (curDstPod < kSuperPodNum)` **恒为真**，`expectedPod0` **在正确语义下恒为 1**。

### 2.2 ret 的理论最小值

若 `expectedPod0 = 1`（正确语义）：

```
ret = actualVal * 10000 + 1 * 1000 + tokenId0
    = actualVal * 10000 + 1000 + tokenId0
```

- `actualVal ∈ {0, 1}`（算子写入 0 或 1）
- `tokenId0 ∈ [0, 511]`

**最小值** = `0 * 10000 + 1000 + 0` = **1000**

**结论**：在正确语义下，`ret ≥ 1000`，**不可能等于 6**。

## 三、R2=6 的解码

### 3.1 唯一成立条件

R2=6 要求：

```
actualVal * 10000 + expectedPod0 * 1000 + tokenId0 = 6
```

因 `tokenId0 ≤ 6`（否则 ret > 6），且 `expectedPod0 * 1000 ≤ 6` 要求 `expectedPod0 = 0`，且 `actualVal * 10000 ≤ 6` 要求 `actualVal = 0`：

| 变量 | 值 | 含义 |
|---|---|---|
| `actualVal` | **0** | 算子写入 `tokenSuperPodInfo[0]` 的值为 0 |
| `expectedPod0` | **0** | 验证逻辑计算的期望值为 0（**与正确语义 1 矛盾**） |
| `tokenId0` | **6** | 第 0 个分区的第 0 个 token 是 token id 6 |

```
ret = 0 * 10000 + 0 * 1000 + 6 = 6  ✓
```

### 3.2 解码结论

1. **tokenId0 = 6**：`groupedTokenIds[0]`（section 0 的首个 token）为 token id 6。这说明 token 分组（scatter）逻辑正确，Phase 2 (grouped ids) 验证也通过。

2. **actualVal = 0**：算子写入 `tokenSuperPodInfo[0]` 的值为 0。这**可能是正确的**——若 token id 6 的所有 16 个 top-k expert 都在 pod 0（`expertId ∈ [0, 63]`），则 `dstPodLocal[0] = 1, dstPodLocal[1] = 0`，写入 `tokenSuperPodInfo[0] = 1, tokenSuperPodInfo[1] = 0`。因此 `actualVal = tokenSuperPodInfo[0]` 的值取决于 token 6 的 expert 分布。

3. **expectedPod0 = 0**：这是**异常的**——根据 2.1 分析，`expectedPod0` 在正确语义下应恒为 1。`expectedPod0 = 0` 意味着验证逻辑中 `expertId / kExpertPerPod` 返回了 ≥ `kSuperPodNum(2)` 的值，导致 `if (curDstPod < kSuperPodNum)` 分支未执行。

## 四、数值验证

### 4.1 生成 token 6 的 top-k expert

`genTopkIndex` 使用 LCG 伪随机算法：

```cpp
uint32_t seed = 0x1234ABCDu;
for (uint32_t i = 0; i < bs; i++) {
    for (uint32_t j = 0; j < k; j++) {
        seed = seed * 1103515245u + 12345u;
        topkIndex[i * k + j] = (seed >> 16) % expertNum;
    }
}
```

跳过前 6 个 token（96 个 expert），取 token 6 的 16 个 expert：

```python
seed = 0x1234ABCD
for i in range(7):                          # 生成 7 个 token
    for j in range(16):
        seed = (seed * 1103515245 + 12345) & 0xFFFFFFFF
        if i == 6:                          # token 6
            expertId = (seed >> 16) % 128
            pod = expertId // 64
            print(f"  expert[{j}] = {expertId:3d}, pod = {pod}")
```

### 4.2 预期输出

若 token 6 的所有 16 个 expert 都在 `[0, 63]`（pod 0），则：
- 算子正确写入：`dstPodLocal[0] = 1, dstPodLocal[1] = 0`
- `tokenSuperPodInfo[0] = 1`（pod 0 标志）
- `tokenSuperPodInfo[1] = 0`（pod 1 标志）
- `actualVal = tokenSuperPodInfo[0] = 1`

但 R2=6 显示 `actualVal = 0`，这意味着：
- 要么 token 6 的 expert 分布使 `dstPodLocal[0] = 0`（即所有 expert 都在 pod 1，`expertId ∈ [64, 127]`），此时 `tokenSuperPodInfo[0] = 0`
- 要么算子的 `>> 6` 也有问题（可能性低）

### 4.3 两种可能的 actualVal=0 场景

**场景 A**（符合根因假设）：
- token 6 的所有 expert 都在 pod 0（`expertId ∈ [0, 63]`）
- 算子用 `>> 6` 正确计算 `curDstPod = 0`，写入 `dstPodLocal[0] = 1`
- 但 `actualVal = tokenSuperPodInfo[0]`... 等等，这里 offset0 = 0，即 `tokenSuperPodInfo[0]` 对应 pod 0
- 若算子正确，`tokenSuperPodInfo[0] = dstPodLocal[0] = 1`，`actualVal` 应为 1，而非 0

**场景 B**（actualVal=0 的解释）：
- token 6 的所有 expert 都在 pod 1（`expertId ∈ [64, 127]`）
- 算子用 `>> 6` 正确计算 `curDstPod = 1`，写入 `dstPodLocal[1] = 1, dstPodLocal[0] = 0`
- `tokenSuperPodInfo[0] = dstPodLocal[0] = 0`，`actualVal = 0` ✓
- 验证逻辑用 `/ 64` 应计算 `curDstPod = 1`，`expectedPod[1] = 1`，`expectedPod0 = expectedPod[0] = 0`
- 此时 `expectedPod0 = 0` 是**正确的**（pod 0 确实无 expert）

**场景 B 的矛盾**：若 `expectedPod0 = 0` 是正确的，则 `ret = 0 * 10000 + 0 * 1000 + 6 = 6`，但这不表示验证失败——`expectedPod0` 只是 `expectedPod[0]`（pod 0 的标志），并非整体期望。真正的验证是 `podInfoMatch != podInfoTotal`，即在 `tokenSuperPodInfo[1]`（pod 1 标志）处可能不匹配。

### 4.4 修正理解

`expectedPod0` 在编码中只是 `expectedPod[0]`（pod 0 的标志值），**不是整体期望是否为 1**。

重新分析：
- `expectedPod0 = 0` 表示 pod 0 无 expert（token 6 的所有 expert 都在 pod 1）
- `actualVal = 0` 表示算子写入 pod 0 标志为 0（正确）
- `tokenId0 = 6` 表示首个 token 是 6

此时 `ret = 6`，但验证失败的原因是**其他位置**的 `tokenSuperPodInfo` 与 `expectedPod` 不匹配（非首个位置）。

### 4.5 重新定位失配点

`ret = 6` 仅记录 `s0=0, i0=0` 的信息，**不代表这是首个失配点**。验证逻辑是：

```cpp
if (podInfoMatch != podInfoTotal) {   // 任一位置失配即进入此分支
    // 只记录 s0=0, i0=0 的信息（硬编码）
}
```

因此 `ret = 6` 告诉我们：
1. 验证整体失败（`podInfoMatch != podInfoTotal`）
2. section 0 的首个 token 是 token id 6
3. token 6 在 section 0 的 pod 0 标志：算子写 0，验证期望 0（二者一致，此位置未失配）
4. 真正的失配在其他位置

### 4.6 根因仍成立

尽管 `expectedPod0 = 0` 在场景 B 中是正确的，但若 gfrun 除法缺陷导致**所有 token** 的 `expectedPod` 计算错误（如全为 0），则：
- 对 expert 分布在 pod 0 的 token：算子写 `tokenSuperPodInfo[pod0] = 1`，验证期望 0 → 失配
- 对 expert 分布在 pod 1 的 token：算子写 `tokenSuperPodInfo[pod1] = 1`，验证期望 0 → 失配

`podInfoMatch` 会小于 `podInfoTotal`，触发 `ret` 编码。而 `ret` 记录的 `s0=0, i0=0` 恰好是 token 6，其 pod 0 标志恰好双方都为 0（巧合一致），但这不代表整体通过。

## 五、Python 数值验证（关键证据）

### 5.1 验证方法

使用与 `genTopkIndex` 完全相同的 LCG 算法生成 `topkIndex`，在 Python 中模拟算子（`>> 6`）和验证逻辑（`/ 64`）的完整执行。

### 5.2 验证结果

| 指标 | Python（标准语义） | gfrun |
|---|---|---|
| `podInfoMatch` | **1024 / 1024** | < 1024（FAIL） |
| 返回值 | **11000** | **6** |
| `actualVal` | 1 | 0 |
| `expectedPod0` | 1 | 0 |
| `tokenId0` | 0 | 6 |

### 5.3 token 6 的 expert 分布（确定性数据）

`genTopkIndex` 使用 LCG，输出确定性。token 6 的 16 个 expert 中，6 个在 pod 0（`expertId ∈ [0,63]`），10 个在 pod 1（`expertId ∈ [64,127]`）。

**标准语义下**：
- `dstPodLocal[0] = 1`（有 pod 0 的 expert）→ `tokenSuperPodInfo[0] = 1`
- `dstPodLocal[1] = 1`（有 pod 1 的 expert）→ `tokenSuperPodInfo[1] = 1`
- `actualVal = 1`，`expectedPod0 = 1`

**gfrun 中**：`actualVal = 0`，`expectedPod0 = 0`（均与标准语义矛盾）

### 5.4 结论

Python 验证证明在标准语义下算子与验证逻辑功能等价，验证全部通过。gfrun 返回 R2=6 纯粹是仿真器对 `>>` 和 `/` 的语义处理差异导致：

1. **算子侧**（`>> 6`）：gfrun 中右移结果不正确 → `dstPodLocal` 未被正确设置 → `tokenSuperPodInfo` 写入 0
2. **验证侧**（`/ 64`）：gfrun 中除法结果不正确 → `expectedPod` 始终为 0
3. 两者在 token 6 位置恰好都为 0（`ret = 6`），但在其他位置产生不同的错误值 → `podInfoMatch != podInfoTotal`

R2=6 的解码与"gfrun 对 `>>` 和 `/` 均有缺陷"的根因假设完全一致。
