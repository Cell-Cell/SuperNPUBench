# group_token_old podInfo R2=6 Issue 材料索引

本文件夹包含 `group_token_old` 算子 Phase 2 `tokenSuperPodInfo` 验证在 gfrun 中失败（R2=6）的完整 Issue 材料。

## 文件清单

| 文件 | 说明 | 用途 |
|---|---|---|
| `ISSUE.md` | Issue 正文（标准格式） | 直接粘贴到 GitCode Issue 创建页 |
| `evidence_code_diff.md` | 算子实现与验证逻辑的代码差异对照 | 证明 `>>` 与 `/` 的使用差异 |
| `evidence_r2_decode.md` | R2=6 返回值解码分析 + Python 数值验证 | 证明标准语义下全部通过，gfrun 语义差异导致失败 |
| `evidence_sim_report_excerpt.md` | 仿真报告关键摘要 | 含修复记录、验证结果、ELF 段映射 |
| `root_cause_analysis.md` | 根因分析详细说明 | 完整根因假设、排除项、验证方法 |
| `reproduce_steps.md` | 一键复现脚本 | 含编译、运行、根因验证脚本 |

## 核心结论

**问题**：`group_token_old` 算子在 gfrun 功能仿真器上返回 R2=6，Phase 2 的 `tokenSuperPodInfo` 验证失败（5/6 项通过）。

**根因**：gfrun 对 `>>`（右移）和 `/`（整数除法）的语义处理存在缺陷。Python 数值验证证明标准语义下算子与验证逻辑功能等价（1024/1024 全部通过），gfrun 中失败纯粹是仿真器语义差异导致。

**证据链**：
1. 算子用 `>> 6`，验证用 `/ 64`，标准语义下等价
2. Python 模拟（标准语义）：1024/1024 PASS，返回 11000
3. gfrun：FAIL，返回 6（actualVal=0, expectedPod0=0, tokenId0=6）
4. 只有使用 `>>`/`/` 的 pod info 验证失败，其余 5 项均通过

## 使用方法

1. 阅读 `ISSUE.md` 获取 Issue 完整正文
2. 将 `ISSUE.md` 内容粘贴到 GitCode Issue 创建页
3. 附件可作为 Issue 评论补充，或打包为 zip 上传
