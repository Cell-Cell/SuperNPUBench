# 附件 5：一键复现脚本

## 一、环境准备

```bash
#!/bin/bash
set -e

# 编译器路径
export COMPILER_DIR=/mnt/workspace/gitCode/cann/Dev-experience/v300/linx-toolchain-build/output/linx_blockisa_llvm_musl/bin

# 验证编译器
$COMPILER_DIR/clang --version
# 预期输出：clang version 15.0.4, Target: linx64v5-unknown-linux-musl
```

## 二、编译算子

```bash
BENCH=/mnt/workspace/gitCode/cann/Dev-experience/v300/SuperNPUBench
SIM=/mnt/workspace/gitCode/cann/Dev-experience/v300/SuperScalarModel
ELF=$BENCH/benchmark/one-level-arch/output/kernel/group_token_old/elf/group_token_old.elf

cd $BENCH/benchmark/one-level-arch/test/kernel/group_token_old
make TESTCASE=group_token_old PLAT=linx
```

### 实际执行的命令（由 Makefile 展开）

```bash
# 步骤 1：编译 .cpp → .o
clang++ -c -mlxbc -fenable-matrix -O2 \
  -mllvm -enable-all-vector-as-tilereg=true \
  -mllvm -linxv5-enable-HL-Inst-Opt=true \
  -mllvm -linxv5-enable-dim-opt=true \
  -mllvm -linxv5-enable-ldst-bridge=false \
  -mllvm -linxv5-enable-continuous-mem-opt=true \
  -mllvm -linxv5-enable-tile-clock-hand=false \
  -mllvm -linxv5-enable-simt-clock-hand=true \
  -mllvm -enable-misched=false \
  -std=c++20 \
  -I.../include -I.../test/common -I.../test/common/src -I.../kernels -I.../models \
  -D__linx -DENABLE_TENSOR_INSTR \
  group_token_old.cpp -o group_token_old.o

# 步骤 2：链接 .o + _start.s → .elf
clang++ -nostartfiles _start.s group_token_old.o -o group_token_old.elf
```

### 编译产物

```
output/kernel/group_token_old/
├── src/
│   └── group_token_old.o
└── elf/
    └── group_token_old.elf    (ELF 64-bit LSB, arch 0x105, statically linked)
```

## 三、功能仿真（复现 R2=6）

```bash
cd $SIM
bin/gfrun -f $ELF
```

### 预期输出

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

**判定**：`R2 = 6` 表示 podInfo 验证失败（5/6 项通过）。

## 四、根因验证脚本（可选）

### 方法 A：验证逻辑改用右移

```bash
# 备份原文件
cp $BENCH/benchmark/one-level-arch/test/kernel/group_token_old/src/group_token_old.cpp \
   $BENCH/benchmark/one-level-arch/test/kernel/group_token_old/src/group_token_old.cpp.bak

# 将 / kExpertPerPod 改为 >> 6（两处：第 261 行和第 293 行）
sed -i 's|expertId / kExpertPerPod|expertId >> 6|g' \
  $BENCH/benchmark/one-level-arch/test/kernel/group_token_old/src/group_token_old.cpp

# 重新编译并运行
cd $BENCH/benchmark/one-level-arch/test/kernel/group_token_old
make TESTCASE=group_token_old PLAT=linx

cd $SIM
bin/gfrun -f $ELF

# 预期：R2 = 0（全部 PASS），确认根因为 gfrun 整数除法缺陷

# 恢复原文件
mv $BENCH/benchmark/one-level-arch/test/kernel/group_token_old/src/group_token_old.cpp.bak \
   $BENCH/benchmark/one-level-arch/test/kernel/group_token_old/src/group_token_old.cpp
```

### 方法 B：算子改用除法

```bash
# 备份原文件
cp $BENCH/benchmark/one-level-arch/kernels/group_token_old/group_token_old.hpp \
   $BENCH/benchmark/one-level-arch/kernels/group_token_old/group_token_old.hpp.bak

# 将 >> 6 改为 / expertPerPod
sed -i 's|topkIndex\[j\] >> 6|topkIndex[j] / expertPerPod|g' \
  $BENCH/benchmark/one-level-arch/kernels/group_token_old/group_token_old.hpp

# 重新编译并运行
cd $BENCH/benchmark/one-level-arch/test/kernel/group_token_old
make TESTCASE=group_token_old PLAT=linx

cd $SIM
bin/gfrun -f $ELF

# 预期：R2 仍为 6 或其他非 0 值，确认 gfrun 除法缺陷影响算子侧

# 恢复原文件
mv $BENCH/benchmark/one-level-arch/kernels/group_token_old/group_token_old.hpp.bak \
   $BENCH/benchmark/one-level-arch/kernels/group_token_old/group_token_old.hpp
```

### 方法 C：最小用例

```bash
# 创建最小测试用例
mkdir -p /tmp/div_test
cat > /tmp/div_test/div_test.cpp << 'EOF'
#include <cstdint>
int main() {
    uint32_t a = 100;
    uint32_t b = a / 64;   // 期望 1
    return b;              // R2 应为 1
}
EOF

# 编译
$COMPILER_DIR/clang++ -c -mlxbc -O2 -std=c++20 -D__linx \
  /tmp/div_test/div_test.cpp -o /tmp/div_test/div_test.o

# 需要 _start.s，可从 group_token_old 工程复制
cp $BENCH/benchmark/one-level-arch/test/kernel/group_token_old/_start.s /tmp/div_test/

$COMPILER_DIR/clang++ -nostartfiles /tmp/div_test/_start.s /tmp/div_test/div_test.o \
  -o /tmp/div_test/div_test.elf

# 运行
cd $SIM
bin/gfrun -f /tmp/div_test/div_test.elf

# 预期：R2 = 1（正确），若为 0 或其他值则确认 gfrun 整数除法缺陷
```

## 五、一键完整复现

```bash
#!/bin/bash
set -e

export COMPILER_DIR=/mnt/workspace/gitCode/cann/Dev-experience/v300/linx-toolchain-build/output/linx_blockisa_llvm_musl/bin
BENCH=/mnt/workspace/gitCode/cann/Dev-experience/v300/SuperNPUBench
SIM=/mnt/workspace/gitCode/cann/Dev-experience/v300/SuperScalarModel
ELF=$BENCH/benchmark/one-level-arch/output/kernel/group_token_old/elf/group_token_old.elf

echo "=== 1. 编译算子 ==="
cd $BENCH/benchmark/one-level-arch/test/kernel/group_token_old
make TESTCASE=group_token_old PLAT=linx

echo "=== 2. 功能仿真（预期 R2=6）==="
cd $SIM
bin/gfrun -f $ELF

echo "=== 复现完成 ==="
echo "若输出末尾为 'R2 = 6'，则问题已复现。"
```

## 六、产物路径汇总

| 产物 | 绝对路径 |
|---|---|
| 算子源码 | `/mnt/workspace/gitCode/cann/Dev-experience/v300/SuperNPUBench/benchmark/one-level-arch/kernels/group_token_old/group_token_old.hpp` |
| 测试驱动 | `/mnt/workspace/gitCode/cann/Dev-experience/v300/SuperNPUBench/benchmark/one-level-arch/test/kernel/group_token_old/src/group_token_old.cpp` |
| 目标文件 | `/mnt/workspace/gitCode/cann/Dev-experience/v300/SuperNPUBench/benchmark/one-level-arch/output/kernel/group_token_old/src/group_token_old.o` |
| ELF 文件 | `/mnt/workspace/gitCode/cann/Dev-experience/v300/SuperNPUBench/benchmark/one-level-arch/output/kernel/group_token_old/elf/group_token_old.elf` |
| 功能仿真器 | `/mnt/workspace/gitCode/cann/Dev-experience/v300/SuperScalarModel/bin/gfrun` |
| 时序仿真器 | `/mnt/workspace/gitCode/cann/Dev-experience/v300/SuperScalarModel/bin/gfsim` |
| 编译器 | `/mnt/workspace/gitCode/cann/Dev-experience/v300/linx-toolchain-build/output/linx_blockisa_llvm_musl/bin/clang++` |
