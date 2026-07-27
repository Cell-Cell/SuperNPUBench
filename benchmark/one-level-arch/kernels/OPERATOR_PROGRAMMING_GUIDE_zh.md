# PTO 一层算子编程手册

> 面向 SuperNPUBench `one-level-arch` 的 C++ 算子开发者。本手册把
> DavinciOO PE-local intrinsic 语义、Linx-TileOP-API 编程接口和
> SuperNPUBench kernel 实践串成一条可执行的开发路径。
>
> 验证边界：本文只把“源码核对、Linx 目标编译、反汇编”标为已验证。
> 未执行 CPU-SIM、QEMU/gfsim 或真实硬件数值与性能测试。

## 目录

- [0. 手册定位与证据口径](#0-手册定位与证据口径)
- [1. 五分钟写出第一个算子](#1-五分钟写出第一个算子)
- [2. 编程模型](#2-编程模型)
- [3. Tile、GlobalTensor 与布局](#3-tileglobaltensor-与布局)
- [4. TileOP API 分类参考](#4-tileop-api-分类参考)
- [5. 从算子语义到 kernel](#5-从算子语义到-kernel)
- [6. 典型算子模式](#6-典型算子模式)
- [7. 编译与反汇编](#7-编译与反汇编)
- [8. 常见错误与排查](#8-常见错误与排查)
- [9. 静态优化方法](#9-静态优化方法)
- [10. 新增算子与测试](#10-新增算子与测试)
- [附录 A：接口速查](#附录-a接口速查)
- [附录 B：验证状态定义](#附录-b验证状态定义)

---

## 0. 手册定位与证据口径

### 0.1 三层来源

| 层次 | 本地来源 | 本手册使用方式 |
| --- | --- | --- |
| ISA / 架构语义 | `/Users/liyi/Documents/GitHub/DavinciOO/isa/intrinsic` | 指令语义、Block family、header、TReg lifetime、异常和编码约束 |
| C++ API / lowering | `/Users/liyi/Documents/GitHub/linx-toolchain-build/src/Linx-TileOP-API` | 可调用签名、Tile 类型、布局、`static_assert` 和 inline-asm/builtin lowering |
| kernel / 测试 | 本目录与 `../test/kernel` | 真实分块、尾块、地址迭代、复用、测试和反汇编入口 |

本次基线如下：

| 来源 | 基线 |
| --- | --- |
| DavinciOO intrinsic 最近提交 | `230ef61`（2026-07-09） |
| Linx-TileOP-API | `6338201`（2026-07-14） |
| SuperNPUBench | `c9cfac3`（2026-07-15） |

固定来源入口：

- [DavinciOO intrinsic 总览](https://github.com/hengliao1972/DavinciOO/blob/230ef61/isa/intrinsic/README.md)
- [DavinciOO 编程模型](https://github.com/hengliao1972/DavinciOO/blob/230ef61/isa/intrinsic/PROGRAMMING_MODEL.md)
- [Linx-TileOP-API 入口](https://github.com/LinxISA/Linx-TileOP-API/blob/6338201/README.md)
- [Linx-TileOP-API 使用文档](https://github.com/LinxISA/Linx-TileOP-API/tree/6338201/docs/tileop-usage)
- [SuperNPUBench kernel 导航](README.md)

### 0.2 冲突时的优先级

三层资料不处在同一抽象层，不能用“名称相同”代替契约核对：

1. 指令架构可见语义和 Block 编码以 DavinciOO intrinsic 页面为准。
2. 当前 C++ 能否调用、参数顺序和编译期约束以 TileOP 头文件为准。
3. SuperNPUBench 只证明某种工程写法存在；只有通过本地编译和反汇编的实例才标为“已编译/已反汇编”。
4. 三者冲突时，在[覆盖矩阵](OPERATOR_COVERAGE_MATRIX.csv)中记录，不静默选择一边。

当前环境有一个需要特别注意的漂移：源码目录与已安装到工具链中的
`jcore/template_asm.hpp` 不完全相同。SuperNPUBench 的正常编译会优先使用工具链内置
`tileop-api`。当前差异集中在 14 个 row/col expand API 和 `TCONCAT` 的 dst dtype
`static_assert`：源码引用 `tile_shape_out::TileDType`，installed header 引用
`tile_shape_out::DType`。因此，本文把“源码 API 存在”和“当前工具链编译通过”作为两个独立状态。

### 0.3 范围

本文覆盖：

- single-PE / PE-local 一层 Tile 编程；
- TLSU、TEPL、CUBE 三个主要 Block family；
- `Tile`、`global_tensor`、`global_iterator`；
- elementwise、归约/广播、layout、gather/scatter、matmul 和组合算子；
- Linx 编译与 `llvm-objdump` 反汇编。

本文不覆盖：

- `GMMA/GLOAD/GSTORE/GBAR` 等跨 PE 或 thread-group 指令；
- PTO 通信 ISA；
- CPU-SIM；
- QEMU/gfsim、真实硬件数值正确性或性能结论；
- 修改 ISA、TileOP API 或现有 kernel 的实现。

---

## 1. 五分钟写出第一个算子

下面的例子完成固定 `16×16 FP32` 矩阵逐元素加法。物理 Tile 大小为 1 KB，
每行 64 B，满足当前一层约束。

```cpp
#include <common/pto_tileop.hpp>

using namespace pto;

void add_16x16(float *out, float *lhs, float *rhs) {
  using GM = global_tensor<float, RowMajor<16, 16>>;
  using VT = Tile<Location::Vec, float, 16, 16, BLayout::RowMajor>;

  GM g_out(out);
  GM g_lhs(lhs);
  GM g_rhs(rhs);

  VT t_lhs;
  VT t_rhs;
  VT t_out;

  TLOAD(t_lhs, g_lhs);
  TLOAD(t_rhs, g_rhs);
  TADD(t_out, t_lhs, t_rhs);
  TSTORE(g_out, t_out);
}
```

这段代码对应四个 Block：

```text
TLOAD  -> BSTART.TLSU + B.DIM + B.IOT + B.IOR
TLOAD  -> BSTART.TLSU + B.DIM + B.IOT + B.IOR
TADD   -> BSTART.TEPL + B.DIM + B.IOT
TSTORE -> BSTART.TLSU + B.DIM + B.IOT + B.IOR
```

写一层算子时，始终先回答四个问题：

1. 全局张量的逻辑 shape、stride 和 layout 是什么？
2. 单个 Tile 的物理 shape、有效区域和 dtype 是什么？
3. 计算能否由现有 TileOP 组合表达？
4. 完整块、行尾、列尾和右下角尾块分别怎样处理？

---

## 2. 编程模型

### 2.1 PE-local Block

一个 TileOP 通常 lowering 为一个 PE-local Block。Block header 描述：

- `BSTART.TLSU/TEPL/CUBE`：选择执行 family 和 opcode；
- `B.DATR`：dtype、padding、round、saturation、compare 等属性；
- `B.DIM`：valid row、valid col、row stride 或 M/N/K；
- `B.IOT`：Tile 输入输出、输出 size class 和 `.reuse`；
- `B.IOR`：地址、stride 或普通标量操作数；
- `B.IOD`：Block 依赖，不等同于跨 PE memory fence。

C++ 程序员通常不手写这些 header，但必须理解它们，因为 shape、dtype、Tile lifetime
和地址错误最终都会体现在 header 或编译器诊断中。

### 2.2 TReg 与 lifetime

DavinciOO active profile 使用四个相对索引队列：

| Queue | 名字 | 典型用途 |
| --- | --- | --- |
| T | `T#1..T#16` | 短生命周期通用中间值 |
| U | `U#1..U#16` | 长生命周期输入或第二条数据流 |
| M | `M#1..M#16` | 多输入、mask、index 或矩阵相关流 |
| N | `N#1..N#16` | 额外中间值或隔离数据流 |

`T#1` 表示当前 T queue 中最新的 live value，不是固定物理寄存器。source 上的
`.reuse` 表示本次消费后仍保持 live。没有 `.reuse` 的 source 可在消费后释放。

在 C++ 中，`.reuse` 往往由编译器根据变量后续使用推导。反汇编中应检查：

- 被多次消费的 A/weight 是否出现 `.reuse`；
- 短生命周期结果是否及时释放；
- 单一 queue 是否因过多 live value 超过 16-entry 命名窗口。

### 2.3 CELL 与 Tile size

当前 active profile 的最小 CELL 为 128 B，Tile allocation 只允许：

| `B.IOT.imm4` | Tile 大小 |
| ---: | ---: |
| 3 | 128 B |
| 4 | 256 B |
| 5 | 512 B |
| 6 | 1 KB |
| 7 | 2 KB |
| 8 | 4 KB |
| 9 | 8 KB |

32 B、64 B 以及 16 KB 以上 size class 在当前 DavinciOO PE-local profile 中不合法。
TileOP 头文件通过 `tile_type_traits::IsValidActiveSize` 对许多一层接口做编译期限制。

### 2.4 ACC

CUBE 的结果先写入 `Location::Acc`，不属于普通 `T/U/M/N` Tile：

```cpp
using A = TileLeft<__half, 64, 64>;
using B = TileRight<__half, 64, 64>;
using C = TileAcc<float, 64, 64>;
using O = Tile<Location::Vec, float, 64, 64, BLayout::RowMajor>;

A a;
B b;
C acc;
O out;

TMATMUL(acc, a, b);
ACCCVT(out, acc);
```

`TMATMUL_ACC(acc, a, b)` 表示在现有 ACC 上继续累加。不能把普通 Vec Tile 当作 CUBE
累加器，也不能把 ACC 直接交给普通 elementwise API。

---

## 3. Tile、GlobalTensor 与布局

### 3.1 `Tile` 模板

```cpp
Tile<Location,
     DType,
     Rows,
     Cols,
     BLayout,
     ValidRows,
     ValidCols,
     SLayout,
     SFractalSize,
     PadValue>
```

最常用的 Vec Tile：

```cpp
using Full = Tile<Location::Vec, float, 16, 16, BLayout::RowMajor>;
using Tail = Tile<Location::Vec, float, 16, 16,
                  BLayout::RowMajor, 13, 9>;
```

`Rows×Cols` 决定物理 allocation；`ValidRows×ValidCols` 决定当前运算的有效区域。
尾块不应通过缩小物理 Tile 到非法 size 来表达，应保留合法物理 Tile，并缩小 valid region。

### 3.2 对齐规则

对于 `SLayout::NoneBox`：

- RowMajor Tile：`Cols × dtype_bits` 必须是 256 bit（32 B）的整数倍；
- ColMajor Tile：`Rows × dtype_bits` 必须是 256 bit 的整数倍。

例如 FP32 RowMajor 的列数应为 8 的倍数，FP16/BF16 应为 16 的倍数，INT8 应为 32 的倍数。
这条规则约束物理行宽，不要求 valid col 也对齐。

### 3.3 `global_tensor`

```cpp
using GRow = global_tensor<float, RowMajor<128, 256>>;
using GCol = global_tensor<float, ColMajor<128, 256>>;

GRow row_view(ptr);
GCol col_view(ptr);
```

`RowMajor<R,C,Stride>` 和 `ColMajor<R,C,Stride>` 可表达带静态 leading stride 的二维视图。
存在动态维度时，构造函数还需要 runtime stride 或 runtime rows/cols。

不要把逻辑 shape 与物理 stride 混为一谈。`TLOAD/TSTORE` 的地址和 stride 进入 TLSU
header；错误的 stride 即使编译通过，也会产生错误的数据视图。

### 3.4 `global_iterator`

`global_iterator<Global, Tile>` 用于二维分块：

```cpp
using GM = global_tensor<float, RowMajor<M, N>>;
using VT = Tile<Location::Vec, float, TM, TN, BLayout::RowMajor>;

global_iterator<GM, VT> src_iter(src);
auto block = src_iter(tile_m, tile_n);
```

当前实现只支持二维 `global_tensor` 迭代。高维算子通常把外层维度放到普通标量循环，
或先把高维索引折叠为二维/一维地址计算。

### 3.5 物理布局与逻辑布局

`BLayout` 描述 Tile 外层行列顺序，`SLayout` 描述 boxed/fractal 内层布局。
矩阵乘推荐使用别名：

- `TileLeft<T,M,K>`：Left operand；
- `TileRight<T,K,N>`：Right operand；
- `TileAcc<T,M,N>`：ACC result。

不要仅凭名字假设 GM 与 Tile 布局相同。`TCOPYIN/TCOPYOUT`、`TLOAD/TSTORE` 和
layout helper 可能产生 `ND2NZ`、`DN2ZN` 等转换。反汇编中的 `B.DATR` 和 builtin
profile 是确认实际转换的最终证据。

---

## 4. TileOP API 分类参考

### 4.1 推荐命名与兼容命名

SuperNPUBench 的 `*_pto.hpp` 以一层架构名为主：

| 推荐写法 | 兼容/通用包装 | 说明 |
| --- | --- | --- |
| `TLOAD/TSTORE` | `TCOPYIN/TCOPYOUT` | 前者直接体现 TLSU；后者可能根据 backend/layout 选择实现 |
| `TMATMUL` | `MATMUL` | 都表达 `C=A×B`，接口实现路径不同 |
| `TMATMUL_ACC` | `MATMACC` | ACC 累加 |
| `TMATMUL_MX` | `MATMULMX/MATMACCMX` | microscaling 变体 |
| `TCVT` | `TCAST` | `TCVT` 是当前一层 opcode 名；`TCAST` 是通用包装 |
| `TTRANS` | 无稳定通用别名 | 某些历史版本要求额外 tmp Tile，使用前核对头文件 |

新 kernel 优先使用当前 `*_pto.hpp` 已采用的一层名字；维护旧 kernel 时不要为了统一风格
机械替换，因为包装 API 可能承担 layout 或 backend 选择。

### 4.2 TLSU：全局内存与 Tile

| API | 典型签名 | 语义 |
| --- | --- | --- |
| `TLOAD` | `TLOAD(tile&, global&)` | GM → Tile |
| `TSTORE` | `TSTORE(global&, tile&)` | Tile → GM |
| `MGATHER` | `MGATHER(dst, gm, offset_tile)` | byte offset 离散加载 |
| `MSCATTER` | `MSCATTER(gm, src, offset_tile)` | byte offset 离散写回 |
| `MGATHER_MASK` | `MGATHER_MASK(dst, gm, offset, mask)` | 带 mask gather |
| `MSCATTER_MASK` | `MSCATTER_MASK(gm, src, offset, mask)` | 带 mask scatter |

必须区分：

- `MGATHER/MSCATTER` 访问 global memory；
- `TGATHER/TSCATTER` 在 Tile 内部按索引重排；
- offset 是 Tile，不能直接传普通指针；
- 当前 kernel 中的 offset 通常是 byte offset，计算时要乘 `sizeof(DType)`。

### 4.3 TEPL Mode 0：Tile-Tile 与一元操作

| 类别 | API |
| --- | --- |
| 算术 | `TADD TSUB TMUL TDIV TREM TFMOD TMAX TMIN` |
| 位运算 | `TAND TOR TXOR TSHL TSHR` |
| 比较/选择 | `TCMP TSEL TPRELU` |
| 一元数学 | `TABS TNOT TNEG TEXP TLOG TRECIP TSQRT TRSQRT TRELU` |
| 三输入/融合 | `TADDC TSUBC TFMA` |
| 类型转换 | `TCVT` |

同 shape 二元操作通常使用统一 `tile_shape` 模板参数，意味着 dst/src 的 dtype、物理 shape
和 layout 必须一致。不同 dtype 的转换要使用 `TCVT`，不同 shape 的广播要使用专用
`TROWEXPAND*`/`TCOLEXPAND*`。

### 4.4 TEPL Mode 1：Tile-Scalar

| 类别 | API |
| --- | --- |
| 标量算术 | `TADDS TSUBS TMULS TDIVS TREMS TFMODS` |
| 标量位运算 | `TANDS TORS TXORS TSHLS TSHRS` |
| clamp/compare | `TMAXS TMINS TCMPS TLRELU` |
| 复合 | `TAXPY TADDSC TSUBSC TSELS` |
| 生成 | `TEXPANDS` |

scalar 经 `B.IOR` 进入 Block。当前头文件会对部分常量使用 anti-fold 处理，避免立即数被
折叠到 zero register 后无法匹配目标指令。

### 4.5 TEPL Mode 2：归约与广播

| 方向 | 归约 | 广播/扩展运算 |
| --- | --- | --- |
| Row | `TROWSUM TROWMAX TROWMIN TROWPROD TROWARGMAX TROWARGMIN` | `TROWEXPAND`、`TROWEXPAND{ADD,SUB,MUL,DIV,MAX,MIN,EXPDIF}` |
| Col | `TCOLSUM TCOLMAX TCOLMIN TCOLPROD TCOLARGMAX TCOLARGMIN` | `TCOLEXPAND`、`TCOLEXPAND{ADD,SUB,MUL,DIV,MAX,MIN,EXPDIF}` |

Row reduction 沿列归约，每行产生一个结果；Col reduction 沿行归约，每列产生一个结果。
广播 API 的 `src0` 与 dst 同形，`src1` 是 `R×1` 或 `1×C` 的归约结果/缩放向量。
三者 dtype 必须一致，但 shape 刻意不同。

### 4.6 TEPL Mode 3：复杂操作

| 类别 | API |
| --- | --- |
| 拼接/子块 | `TCONCAT TEXTRACT TINSERT` |
| 图像/layout | `TIMG2COL TFILLPAD TTRANS` |
| 序列/随机 | `TCI TTRI TRANDOM` |
| 量化 | `TQUANT TDEQUANT` |
| 排序/统计 | `TSORT32 TMRGSORT THISTOGRAM` |
| Tile 内索引 | `TGATHER TSCATTER TGATHERB` |
| partial-valid | `TPARTADD TPARTMUL TPARTMAX TPARTMIN` |

这组接口的 profile-specific 约束最多。使用前必须同时核对 ISA 页面、当前签名和已有 kernel。
特别是排序结果布局、predicate 传递、量化 scale layout 和 histogram 分箱语义，不能仅凭
函数名推断。

### 4.7 CUBE

| API | 语义 |
| --- | --- |
| `TMATMUL` | `C=A×B`，C 为 ACC |
| `TMATMUL_ACC` | `C+=A×B` |
| `TMATMUL_BIAS` | `C=A×B+bias` |
| `TMATMUL_MX` | 带 A/B scale 的 microscaling matmul |
| `ACCCVT` | ACC → 普通 Tile，可伴随 dtype/layout conversion |

DavinciOO ISA 还记录 `TGEMV*`，但当前一层 `template_asm.hpp` 没有同名公开接口；覆盖矩阵
将其标为 ISA-only，而不是声称可以直接调用。

---

## 5. 从算子语义到 kernel

### 5.1 第一步：标准化语义

把框架算子写成明确的标量或张量公式，并固定：

- 输入/输出 shape；
- dtype 和 accumulator dtype；
- layout、stride、broadcast 轴；
- padding、舍入、饱和和 NaN/Inf 约定；
- 尾块是否允许 partial valid；
- 是否存在跨 Tile 归约或顺序依赖。

如果公式中存在“每元素访问不同 GM 地址”，优先考虑 `MGATHER/MSCATTER`；如果只是在同一
Tile 内重排，考虑 `TGATHER/TSCATTER/TTRANS`。

### 5.2 第二步：选择 Tile

Tile 选择必须同时满足：

1. 物理大小落在 128 B–8 KB；
2. 行/列满足 32 B 对齐；
3. CUBE 的 M/N/K 和 fractal layout 合法；
4. live Tile 数与总 CELL 容量可接受；
5. 尾块可用 valid region 表达。

常见做法是先从 1–4 KB Tile 开始，编译和反汇编稳定后再放大。

### 5.3 第三步：建立 GM 视图

二维连续张量使用 `global_tensor + global_iterator`。高维张量通常采用：

- 外层维度用标量循环；
- 内层连续二维切片用 `global_tensor`；
- 不规则映射生成 offset Tile 后用 `MGATHER/MSCATTER`。

不要在 Tile 内写 per-element C++ 循环模拟 SIMD；这会丢失一层 Tile ISA 的价值。

### 5.4 第四步：画 Tile 数据流

以稳定 softmax 为例：

```text
input
  ├─ TROWMAX ───────────────┐
  └─ TROWEXPANDSUB(x,max)   │
          ↓                 │
         TEXP               │
          ├─ TROWSUM ───────┤
          └─ TROWEXPANDDIV ─┘ -> output
```

数据流图能提前暴露：

- 哪些 Tile 需要多次消费；
- 哪些 source 应在反汇编中带 `.reuse`；
- 哪些值是 `R×C`、`R×1` 或 `1×C`；
- 是否需要跨 Tile 二次归约。

### 5.5 第五步：完整块与尾块分离

常见二维分块需要四类实例：

1. 完整 `TM×TN`；
2. 尾行 `valid_m×TN`；
3. 尾列 `TM×valid_n`；
4. 右下角 `valid_m×valid_n`。

用 `if constexpr` 消除不存在的尾块分支，为每种 valid region 定义单独 Tile 类型。不要把
runtime 余数强塞给只支持静态 valid shape 的接口。

### 5.6 第六步：先编译，再看反汇编

编译通过只说明模板、约束和目标 lowering 接受该程序。还需在 `.diss` 中确认：

- family 和 opcode 正确；
- `B.DIM` 使用预期 valid shape/stride；
- `B.IOT` 输入输出角色和 size class 正确；
- `B.IOR` 地址、stride、scalar 数量正确；
- 复用和 ACC 链符合数据流。

---

## 6. 典型算子模式

### 6.1 GELU：elementwise 链

参考：[element_wise/gelu_pto.hpp](element_wise/gelu_pto.hpp)。

GELU 的工程价值不在公式本身，而在展示：

- BF16/FP16 输入先 `TCVT` 到 FP32 中间值；
- clamp 使用 `TMAXS/TMINS`；
- 多项式或 tanh 近似由 `TMUL/TMULS/TADDS` 组合；
- 原输入在多次消费时触发 `.reuse`；
- 末尾再 `TCVT` 回输出 dtype。

本地 BF16 exact 配置已用于 Linx 编译与反汇编基线。反汇编可观察到 TLSU load/store、
`TCVT`、`TMAXS`、`TMINS`、`TMUL`、`TMULS` 和 `TADDS`。

### 6.2 ReduceSum：跨 Tile 二次归约

参考：[reduction/reducesum_rowvec_pto.hpp](reduction/reducesum_rowvec_pto.hpp)。

当逻辑行宽大于单 Tile 宽度时，单条 `TROWSUM` 只能得到局部结果。完整算法需要：

1. 每个 N block `TLOAD`；
2. `TROWSUM` 得到局部 row sum；
3. `TADD` 累加不同 N block 的局部结果；
4. `TSTORE` 输出。

`TROWMAX/TCOLMAX` 等归约遵循同样模式，只是初值和合并运算不同。

### 6.3 Broadcast 与归一化

广播不是把小 Tile 复制成大 Tile 后再算。优先使用专用 expand op：

```cpp
TROWSUM(row_sum, x);
TADDS(denom, row_sum, epsilon);
TROWEXPANDDIV(y, x, denom);
```

这正是 softmax、权重归一化、RMSNorm 和量化 scale 的公共骨架。`src1` 的 shape 与矩阵
不同是正常契约，不要为了通过同 shape 模板而人工填充。

### 6.4 Gather：索引 Tile 与 byte offset

参考：[gather/gather_pto.hpp](gather/gather_pto.hpp)。

典型 embedding gather：

1. 读取 index/offset Tile；
2. 如输入是元素索引，乘 `sizeof(DType)` 转 byte offset；
3. 为当前 N block 调整 GM base；
4. `MGATHER(out_tile, adjusted_gm, offset_tile)`；
5. `TSTORE`。

尾行和尾列必须分别定义 valid Tile。若 offset 可能超过 32 bit，应在模板层拒绝或改用更宽
地址策略，不能依赖溢出行为。

### 6.5 Transpose：原生与通用路径

参考：[transpose/transpose_pto.hpp](transpose/transpose_pto.hpp)。

- 二维块转置：`TLOAD → TTRANS → TSTORE`；
- 任意高维轴交换：生成输出线性索引，按各维 stride 计算输入 byte offset，再 `MGATHER`。

不要把 GPU shared-memory swizzle 机械搬到 PTO。存在原生 `TTRANS` 时，优先让 Tile ISA
承担块内转置；只有任意轴映射才需要显式 offset 算法。

### 6.6 MatMul：CUBE、ACC 与复用

参考：[matmul/matmul_pto.hpp](matmul/matmul_pto.hpp)和
[matmul/matmul.hpp](matmul/matmul.hpp)。

标准骨架：

```text
for m block
  for n block
    load A(m,0), B(0,n)
    TMATMUL(acc,A,B)
    for remaining k blocks
      load/reuse A and B
      TMATMUL_ACC(acc,A,B)
    ACCCVT(out,acc)
    store out(m,n)
```

优化方向由循环顺序决定：

- 固定 m、遍历 n：A 更适合复用；
- 固定 n、遍历 m：B 更适合复用；
- MX 变体要同时规划 data Tile 和 scale Tile；
- ACC 只在 K reduction 完成后导出。

### 6.7 FlashAttention 与 DeepSeek 组合算子

参考：[fa/fa_2d_unroll_pto.hpp](fa/fa_2d_unroll_pto.hpp)。DeepSeek 目录包含量化、MoE、MHC
和 transpose 组合案例，但当前是工作区未跟踪内容，应视为实验性来源，不作为已发布 API 证明。

复杂算子应先拆成稳定骨架：

```text
load -> matmul -> row max -> exp/scale -> row sum -> normalize -> matmul -> store
```

然后再决定是否做 Tile 复用、K/N 双层 unroll、融合量化或布局变换。未经过编译/反汇编的
高级指令链必须标为“source-only”，不能写成已验证实现。

---

## 7. 编译与反汇编

### 7.1 工具链

本地已验证工具链入口：

```bash
export COMPILER_DIR=/Users/liyi/Documents/GitHub/linx-toolchain-build/output/linx_blockisa_llvm_musl/bin
```

SuperNPUBench makefile 的一层 Linx 路径使用：

```text
-D__linx -DENABLE_TENSOR_INSTR
-mlxbc -fenable-matrix -std=c++20
```

### 7.2 本次验证记录

以下项目均在 2026-07-17 使用上述本地工具链完成 `PLAT=linx` 编译和
`llvm-objdump -dl` 反汇编：

| 项目 | 配置 | 反汇编确认 |
| --- | --- | --- |
| GELU | BF16、`tMs=2048`、exact | `TLOAD/TSTORE`、`TCVT`、`TMAXS/TMINS`、`TMUL/TMULS`、`TADDS`、`TEXP/TRECIP` |
| ReduceSum row | INT32、`tM=16`、`tN=128` | `TROWSUM`、`TADD`、`TEXPANDS`、`TMOV`、TLSU load/store |
| Gather | FP32 data、U32 offset、`32×64` Tile | `MGATHER`、`TLOAD U32`、`TSTORE FP32` |
| MatMul | `MASK_FP32`、`M=N=K=256`、`32×32×64` Tile | `TMATMUL`、`TMATMUL.ACC`、`ACCCVT`、TLSU load/store |
| 最小 API probe | FP32 `16×16` matrix、`16×8` row stripe | `TROWEXPANDMUL`、`TTRANS`、TLSU load/store |

最小 probe 使用工具链已安装的 `tileop-api` 编译；它证明当前运行环境可用，不替代源码目录
与 installed header 的差异记录。其临时源文件、对象和反汇编不进入仓库。

### 7.3 单算子编译

以 GELU 为例：

```bash
cd benchmark/one-level-arch/test/kernel/element_wise/gelu
make TESTCASE=gelu DTYPE=__bf16 tMs=2048 \
  'gMs=24*8*1024' SHAPE_NAME=24_8_1024 Approximate=false \
  PLAT=linx COMPILER_DIR="$COMPILER_DIR" all
```

常用目标：

| target | 作用 |
| --- | --- |
| `all` | 编译并链接 ELF |
| `diss` | 编译并生成 `.diss` |
| `sim` | 运行模拟器；不属于本手册验证范围 |
| `clean` | 清理当前测试生成物 |

### 7.4 反汇编

```bash
make ... diss
```

等价核心命令：

```bash
"$COMPILER_DIR/llvm-objdump" -dl kernel.elf > kernel.elf.diss
```

检查顺序：

1. 找到实例化后的 kernel 函数；
2. 搜索 `BSTART.TLSU`，核对 load/store；
3. 搜索 `BSTART.TEPL`，核对 elementwise/reduce/complex opcode；
4. 搜索 `BSTART.CUBE`，核对 matmul/ACC；
5. 向下检查同一 Block 的 `B.DATR/B.DIM/B.IOT/B.IOR`；
6. 将 Tile size、valid region、scalar 和 `.reuse` 回填到源码数据流图。

示意：

```asm
BSTART.TLSU TLOAD, BF16
B.DIM       zero, 2048, ->lb0
B.IOT       [], last, ->t<4KB>
B.IOR       [a0,s3],[]

BSTART.TEPL TCVT, BF16
B.DATR      FP32, byte0, Null
B.IOT       [t#1], last, ->t<8KB>
```

反汇编产物位于 `benchmark/one-level-arch/output/`，属于生成物，不提交。

---

## 8. 常见错误与排查

### 8.1 Tile size 不合法

症状：`static_assert` 指向 `IsValidActiveSize` 或 `TilesizeCode`。

检查 `Rows×Cols×dtype_bits/8` 是否恰好是 128 B、256 B、512 B、1/2/4/8 KB。
不要用 32 B/64 B 小 Tile 表达归约向量；使用合法物理 Tile 加较小 valid region。

### 8.2 行/列未达到 32 B 对齐

症状：`BFractal_ is RowMajor ... 32 bytes align`。

检查物理 Cols/Rows，而不是 valid col/row。尾块仍应保留对齐的物理 shape。

### 8.3 广播 shape 被误设为相同

`TROWEXPAND*` 的 scale/sum Tile 应为 `R×1`，`TCOLEXPAND*` 应为 `1×C`。如果把它们
声明成 `R×C`，即使部分版本能编译，也会掩盖真正的数据契约。

### 8.4 ACC 类型错误

`TMATMUL*` 输出必须是 `Location::Acc`。普通结果必须经过 `ACCCVT`。错误地直接 TSTORE ACC
或对 ACC 调 elementwise 是接口层错误。

### 8.5 `TGATHER` 与 `MGATHER` 混淆

- GM 不规则读取：`MGATHER`；
- Tile 内重排：`TGATHER`；
- offset 单位和位宽必须显式确认。

### 8.6 `TTRANS` 签名漂移

部分历史 kernel 保留 `SUPERNPU_PTO_TTRANS_NEEDS_TMP` 兼容宏。新代码以当前
`template_asm.hpp` 的实际签名为准，不要从旧例子复制 tmp 参数后假设兼容。

### 8.7 源码 API 与已安装工具链不一致

诊断流程：

```bash
diff -qr \
  /Users/liyi/Documents/GitHub/linx-toolchain-build/src/Linx-TileOP-API/include \
  /Users/liyi/Documents/GitHub/linx-toolchain-build/output/linx_blockisa_llvm_musl/lib/clang/15.0.4/include/tileop-api
```

若存在差异：

1. 记录编译实际使用的头文件；
2. 不把 installed-only 成功写成 source commit 已验证；
3. 需要修 API 时另开代码变更，不在文档任务中静默修复。

### 8.8 编译通过不等于结果正确

本手册不运行 CPU-SIM 或硬件。编译与反汇编只能验证：

- C++ 模板与 target backend 接受代码；
- 关键 TileOP lowering 为预期 Block family/opcode；
- header 中可见 shape、size、operand 与地址形式合理。

数值边界、异常值、跨 Tile 合并和性能仍需独立运行验证。

---

## 9. 静态优化方法

### 9.1 先减少 GM 流量

- 多 consumer 的输入只 load 一次；
- 把 elementwise 链放在同一 Tile 生命周期内；
- 归约结果直接喂给 expand op；
- matmul 的 A/B 复用由循环顺序决定。

### 9.2 使用专用指令代替展开

| 目标 | 优先形式 |
| --- | --- |
| 行归一化 | `TROWSUM + TROWEXPANDDIV` |
| 稳定 softmax | `TROWMAX + TROWEXPANDSUB + TEXP + TROWSUM + TROWEXPANDDIV` |
| 2D 转置 | `TTRANS` |
| 融合乘加 | `TFMA` 或 `TMATMUL_ACC` |
| 不规则 GM 读取 | offset Tile + `MGATHER` |

### 9.3 控制 live range

更大的 Tile 不一定更快。它会增加 CELL 占用并压缩可同时存活的中间值数量。反汇编中若
出现过多 `.reuse` 或 queue 跨度变大，应考虑：

- 缩短表达式链；
- 重新安排计算顺序；
- 将长生命周期输入与短中间值分到不同数据流；
- 减小 Tile 或 unroll 因子。

### 9.4 检查编译器是否实现预期复用

源码变量复用不保证最终一定出现理想 `.reuse`。以 `.diss` 为准比较 baseline、reuse-A、
reuse-B 版本，确认 load 数量、`B.IOT` 和 CUBE 链是否真的变化。

### 9.5 不做无运行证据的性能结论

反汇编可支持“指令数更少”“GM load 更少”“出现复用”等静态判断，但不能直接推出实际
latency、throughput 或能耗。本文不把静态差异描述成硬件加速比。

---

## 10. 新增算子与测试

### 10.1 kernel 结构

在 `kernels/<op>/` 增加 header-only 实现：

1. include guard；
2. `#include <common/pto_tileop.hpp>`；
3. 模板参数描述 global shape、Tile shape、dtype 和模式；
4. `static_assert` 固定语义与对齐约束；
5. 完整块与尾块分开；
6. 注释说明公式、Tile 数据流和关键指令。

### 10.2 测试结构

在 `../test/kernel/<op>/` 增加：

```text
<op>/
├── Makefile
├── compile.all
└── src/
    └── <op>.cpp
```

Makefile 只声明 `SRC_FILE`、`TARGET` 和算子参数，然后 include `test/common/Makefile.common`。
`compile.all` 至少保留一个典型配置，并显式传递 `COMPILER_DIR`。

### 10.3 文档验收清单

- [ ] 公式、shape、dtype、layout 和尾块策略明确；
- [ ] 每个 Tile 物理大小合法且行/列对齐；
- [ ] `TGATHER/MGATHER`、普通 Tile/ACC 未混淆；
- [ ] Linx 目标编译成功；
- [ ] 反汇编出现预期 family/opcode；
- [ ] `B.DIM/B.IOT/B.IOR` 与源码一致；
- [ ] 未验证的数值、性能和硬件行为明确标注；
- [ ] 新 API 或 mapping 已更新覆盖矩阵。

---

## 附录 A：接口速查

### A.1 shape 类别

| 类别 | 典型签名 |
| --- | --- |
| Unary | `OP(dst, src)` |
| Binary same-shape | `OP(dst, src0, src1)` |
| Ternary same-shape | `OP(dst, src0, src1, src2)` |
| Tile-scalar | `OP(dst, src, scalar)` |
| Broadcast | `OP(matrix_dst, matrix_src, row_or_col_vector)` |
| GM load | `OP(tile_dst, global_src)` |
| GM store | `OP(global_dst, tile_src)` |
| GM indexed | `OP(tile/global dst, global/tile src, offset_tile)` |
| CUBE | `OP(acc_dst, left, right[, extra])` |

### A.2 family 选择

| 需求 | family / API |
| --- | --- |
| GM ↔ Tile | TLSU：`TLOAD/TSTORE/MGATHER/MSCATTER` |
| 同 shape 逐元素 | TEPL Mode 0 |
| Tile + scalar | TEPL Mode 1 |
| 行列归约/广播 | TEPL Mode 2 |
| layout/排序/量化/Tile 内索引 | TEPL Mode 3 |
| 矩阵乘与 ACC | CUBE |

完整逐项状态见 [OPERATOR_COVERAGE_MATRIX.csv](OPERATOR_COVERAGE_MATRIX.csv)。

## 附录 B：验证状态定义

| 状态 | 含义 |
| --- | --- |
| `ISA` | 有 DavinciOO intrinsic 页面 |
| `API` | 当前 TileOP 源码有公开同名接口 |
| `ALIAS` | 通过通用/兼容包装到另一 API |
| `KERNEL` | SuperNPUBench kernel 中存在调用或明确案例 |
| `COMPILED` | 本次用 Linx 目标工具链完成编译 |
| `DISS` | 本次在反汇编中确认目标指令/Block |
| `SOURCE_ONLY` | 仅源码存在，未完成本次编译/反汇编 |
| `ISA_ONLY` | ISA 有定义，当前 C++ API 无同名入口 |
| `API_ONLY` | API helper/融合入口，无独立同名 ISA 页面 |
| `GAP` | 三层语义、签名或版本存在待解决差异 |
