# repro: gfsim reference model mis-decodes tile stream as bogus ACRC (moe_combine_v2)

SuperScalarModel **`d8903938`**（codex/pr-0.58.4-shared-model）上 `moe_combine_v2`
的 gfsim 崩溃复现材料包。issue 正文见 `ISSUE.md`（可直接粘贴到 GitHub issue）。

## 一句话

`moe_combine_v2` ELF 在**两版 gfrun 上均 R2=0 全过**（1014 blocks / 5472 inst），
但在 **gfsim `d8903938` 的内嵌参考模型中必崩**（`Bad Syscall Request: syscall(20e00,…)`，
exit 134）；**同一 ELF 在 `a5dca25` 上正常完成**（5205 cycles）——单变量锁定模型回归。
最新提交 `687c37b` 仍未修复（断言点暴露显式误解码 `TSTORE → TMOV INVALID`）。

## 目录结构

```
.
├── ISSUE.md                                # issue 正文（现象/根因/复现/矩阵，可直接粘贴）
├── README.md                               # 本文件
├── repro.sh                                # 一键复现: clone → 双版本构建 → 5 ELF 矩阵
├── kernels/
│   ├── moe_combine_v2.hpp                  # 修复版源码（TSUB 替代 TMOV，见文件内注释）
│   └── moe_combine_orig_tmov_tcmp.hpp      # 原始 TMOV/TCMP 版（崩溃与其无关的对照）
├── test/
│   ├── moe_combine_v2.cpp                  # 测试 main（gfrun R2=0 自校验）
│   └── _start.s                            # 裸机启动（_end 处真 ACRC 结束握手）
├── elf/                                    # 5 枚预编译 ELF，无需自备工具链
│   ├── moe_combine_v2_0828tool.elf         # 主复现（TileOP f94bc12 × llvm adcb87948）
│   ├── moe_combine_v2_0824tool.elf         # 编译器对照（a795b97 × 611105f2b）
│   ├── moe_combine_orig_tmov_tcmp_0824tool.elf  # 算子源码对照（原始 TMOV/TCMP 版）
│   ├── min_pack_once_291blocks_PASS.elf    # 梯度二分: pack×1（gfsim PASS）
│   └── min_pack_twice_618blocks_CRASH.elf  # 梯度二分: pack×2（gfsim CRASH，代码逐字相同）
└── evidence/                               # 各场景日志（命名即结论）
```

## 快速复现（3 条命令）

```bash
git clone https://github.com/LinxISA/SuperScalarModel.git && cd SuperScalarModel
git checkout d8903938 && mkdir build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make gfsim gfrun -j
./bin/gfsim -f /path/to/elf/moe_combine_v2_0828tool.elf    # 预期: Bad Syscall, exit 134
./bin/gfrun -t 1 -f /path/to/elf/moe_combine_v2_0828tool.elf   # 对照: R2 = 0
```

切换 `a5dca25` 重新构建后跑同一 ELF → 预期正常完成（5205 cycles）。

全矩阵一键执行：`bash repro.sh`（自动 clone + 双版本构建 + 5 枚 ELF 交叉验证）。
