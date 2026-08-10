# Group Token (PTO one-level-arch)

## 功能

MoE (Mixture of Experts) Token 分组算子，是 MoE Dispatch 的核心环节。
包含两个阶段：

1. **CalTokenPerExpertCnt** — 统计每个 expert 收到的 token 数量（直方图）
2. **GroupToken** — 把 token 按"本地 expert"重排，输出分组后的 token id 列表

数据流：
```
topkIndex [bs, k]  →  CalTokenPerExpertCnt  →  tokenPerExpertCnt [expertNum]
topkIndex [bs, k]  →  GroupToken            →  groupedTokenIds [expertPerRank, bs]
                                                expertSectionTokenCnt [expertPerRank]
```

逻辑参考 `cann-samples/Samples/2_Performance/group_token_story`。

## 输入输出

| 参数 | 类型 | 说明 |
|------|------|------|
| `topkIndex` | `uint32_t*` | 输入 [bs × k]，每个元素是全局 expert id |
| `tokenPerExpertCnt` | `uint32_t*` | 输出 [expertNum]，每个 expert 的 token 计数 |
| `groupedTokenIds` | `uint32_t*` | 输出 [expertPerRank × bs]，按 expert 分区排布的 token id |
| `expertSectionTokenCnt` | `uint32_t*` | 输出 [expertPerRank]，每个分区已写入的 token 数 |

## MoE 拓扑参数（默认值，对应 gen_expert_ids.py）

| 参数 | 值 | 说明 |
|------|------|------|
| `kBS` | 512 | batch size（token 总数） |
| `kTopK` | 16 | 每个 token 选中的 top-k expert 数 |
| `kExpertPerRank` | 4 | 每个 rank 的本地 expert 数 |
| `kRankPerPod` | 16 | 每个 Pod 的 rank 数 |
| `kSuperPodNum` | 2 | 超节点数 |
| `kExpertPerPod` | 64 | 每个 Pod 的 expert 数 |
| `kExpertNum` | 128 | 全局 expert 总数 |

## Tile 类型

| Tile | 类型 | 用途 |
|------|------|------|
| `TileU32` | `Tile<Vec, uint32_t, 16, 16, RowMajor>` | 直方图计数 tile（256 元素，容纳 128 expert） |

## 调用的 TileOp

| 操作 | 说明 |
|------|------|
| `TEXPANDS` | 初始化直方图 tile 为 0 |
| `TSTORE` | 存储直方图到 global memory |
| `TCOPYOUT` | 存储分组结果 |

## 实现方式

### Phase 1: CalTokenPerExpertCnt（直方图）

- **标量路径**（`calTokenPerExpertCnt_scalar`）：遍历 topkIndex，对每个 expert id 计数。
  无 backend 依赖，在 `__linx` / `__cpu_sim__` / `__ARM_FEATURE_SME` 下均正确运行。
- **SIMT 路径**（`CalTokenPerExpertCnt_Impl`，`#ifndef __linx` 守卫）：
  `<<<1, expertNum, 1>>>` 启动，每个 lane 负责一个 expert，线性扫描计数。

### Phase 2: GroupToken（分组 scatter）

- **标量路径**（`groupToken_scalar`）：
  1. 初始化 `expertSectionTokenCnt[0..expertPerRank-1] = 0`
  2. 对每个 token，遍历其 top-k expert id，取 `expertId % expertPerRank` 的最小值作为归属分区
  3. 在该分区用写指针（`expertSectionTokenCnt[minLocalExpId]++`）申请槽位，写入 token id

  与 `sort/topk.hpp` 的 Phase 5 标量 scatter 风格一致。

## 源文件

| 文件 | 说明 |
|------|------|
| `group_token.hpp` | 算子实现（标量 + 可选 SIMT） |

## 参见

- [cann-samples group_token_story](../../../../../cann-samples/Samples/2_Performance/group_token_story)
- [sort/topk.hpp](../sort/topk.hpp) — 类似的直方图 + 标量 scatter 模式
