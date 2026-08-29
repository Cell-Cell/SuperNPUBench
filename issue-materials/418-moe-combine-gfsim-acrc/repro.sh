#!/bin/bash
# ============================================================================
# gfsim 参考模型误解码假 ACRC 复现脚本
# (moe_combine_v2: reference model mis-decodes tile stream as bogus ACRC)
#
# 现象: moe_combine_v2 ELF 在两版 gfrun 上均 R2=0 通过，但在 gfsim
#       d8903938 的内嵌参考模型中必崩 (Bad Syscall, exit 134)。
#       同一 ELF 在 a5dca25 上正常完成 —— 单变量锁定模型回归。
#
# 基座: LinxISA/SuperScalarModel
#         崩溃版: codex/pr-0.58.4-shared-model @ d8903938 (08-27)
#         通过版: exp @ a5dca25 (08-24 构建)
#         最新版: 687c37b (08-29, 仍未修复, 断言点变为
#                 "Local TSTORE requires one legal source Tile descriptor")
# 输入: 本目录自带 5 枚预编译 ELF（无需自备编译器工具链）
#
# 用法:
#   bash repro.sh              # clone + 双版本构建 + 全矩阵复现
#
# 环境变量:
#   SSM_SRC   已有的 SuperScalarModel 源码目录（缺省 clone 到 ./SuperScalarModel）
#   SKIP_BUILD=1               跳过构建（复用已有 bin/gfsim, bin/gfrun）
# ============================================================================
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ELF_DIR="$HERE/elf"
SSM_SRC="${SSM_SRC:-$HERE/SuperScalarModel}"
CRASH_COMMIT="d8903938"   # codex/pr-0.58.4-shared-model
PASS_COMMIT="a5dca25"     # exp

for f in moe_combine_v2_0828tool.elf moe_combine_v2_0824tool.elf \
         moe_combine_orig_tmov_tcmp_0824tool.elf \
         min_pack_once_291blocks_PASS.elf min_pack_twice_618blocks_CRASH.elf; do
    [ -f "$ELF_DIR/$f" ] || { echo "ELF not found: $ELF_DIR/$f"; exit 1; }
done

build_model() {  # $1 = src dir
    (cd "$1" && python3 build.py configure > /dev/null 2>&1 && \
        python3 build.py build --target gfsim --target gfrun -j8 > /dev/null 2>&1) || \
    (cd "$1" && mkdir -p build && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /dev/null && \
        cmake --build build --target gfsim gfrun -j "$(nproc)" > /dev/null)
}

run_case() {  # $1 = bin dir, $2 = elf, $3 = label
    local out; out=$(timeout 300 "$1/gfsim" -f "$2" 2>&1); local rc=$?
    if echo "$out" | grep -q "Total Cycles"; then
        echo "  [PASS] $3  ($(echo "$out" | grep -m1 -oE 'Total Cycles\.{2,}: +[0-9]+'))"
    elif echo "$out" | grep -q "Bad Syscall"; then
        echo "  [CRASH] $3  -> $(echo "$out" | grep -m1 -oE 'Bad Syscall Request: syscall\([^;]*'))"
    else
        echo "  [CRASH] $3  -> $(echo "$out" | grep -m1 -E 'ASSERTION FAILED[^,]*' | head -c 120)  (rc=$rc)"
    fi
}

# ---------- [1] 获取模型源码 ----------
if [ "${SKIP_BUILD:-0}" = "1" ]; then
    echo "===== [1/3] SKIP_BUILD=1, 复用已构建模型 ====="
else
    if [ ! -d "$SSM_SRC" ]; then
        echo "===== [1/3] clone SuperScalarModel ====="
        git clone https://github.com/LinxISA/SuperScalarModel.git "$SSM_SRC" || exit 1
    fi

    # ---------- [2] 构建崩溃版 d8903938 与通过版 a5dca25 ----------
    echo "===== [2/3] build crash rev $CRASH_COMMIT ====="
    git -C "$SSM_SRC" checkout --force "$CRASH_COMMIT" || exit 1
    build_model "$SSM_SRC" || exit 1
    CRASH_BIN="$SSM_SRC/bin"

    echo "===== [2/3] build pass rev $PASS_COMMIT ====="
    git -C "$SSM_SRC" checkout --force "$PASS_COMMIT" || exit 1
    build_model "$SSM_SRC" || exit 1
    PASS_BIN="$SSM_SRC/bin"
fi

# ---------- [3] 矩阵复现 ----------
echo "===== [3/3] 复现矩阵 ====="
MAIN="$ELF_DIR/moe_combine_v2_0828tool.elf"

echo "--- A. 主复现 (moe_combine_v2 × 0828 工具链) ---"
run_case "$CRASH_BIN" "$MAIN" "gfsim d8903938 (预期 CRASH: Bad Syscall)"
run_case "$PASS_BIN"  "$MAIN" "gfsim a5dca25  (预期 PASS)"

echo "--- B. gfrun 对照 (同一 ELF, 功能模型无恙) ---"
timeout 90 "$CRASH_BIN/gfrun" -t 1 -f "$MAIN" 2>&1 | grep -oE "R2 = 0" | tail -1 | sed 's/^/  [gfrun d8903938] /'
timeout 90 "$PASS_BIN/gfrun"  -t 1 -f "$MAIN" 2>&1 | grep -oE "R2 = 0" | tail -1 | sed 's/^/  [gfrun a5dca25 ] /'

echo "--- C. 编译器无关 (同源码 × 0824 工具链 ELF) ---"
run_case "$CRASH_BIN" "$ELF_DIR/moe_combine_v2_0824tool.elf" "gfsim d8903938 (预期 CRASH)"

echo "--- D. 算子源码无关 (原始 TMOV/TCMP 版 ELF, 0824 工具链) ---"
run_case "$CRASH_BIN" "$ELF_DIR/moe_combine_orig_tmov_tcmp_0824tool.elf" "gfsim d8903938 (预期 CRASH)"

echo "--- E. 梯度二分 (pack×1 vs pack×2, 代码逐字相同仅调用次数翻倍) ---"
run_case "$CRASH_BIN" "$ELF_DIR/min_pack_once_291blocks_PASS.elf"  "gfsim d8903938 (预期 PASS)"
run_case "$CRASH_BIN" "$ELF_DIR/min_pack_twice_618blocks_CRASH.elf" "gfsim d8903938 (预期 CRASH)"

echo ""
echo "完成。崩溃版参考模型 TPC 轨迹见 evidence/E7_*.txt（4 字节指令中间 +2 错位解码证据）。"
