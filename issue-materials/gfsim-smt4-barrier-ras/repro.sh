#!/bin/bash
# Repro: gfsim (timing model) cannot run 4-PE SPMD barrier kernels
# LinxISA/SuperScalarModel issue materials — gfsim SMT4 RAS crash / barrier hang
#
# Prereq: SuperScalarModel built at the affected commit (verified on
#         codex/pr-0.58.4-shared-model @ d8903938, the 0828 workspace build).
#         GFSIM=<path-to-gfsim>, GFRUN=<path-to-gfrun> (optional overrides).
#
# All ELFs are prebuilt (TileOP-API f94bc12 x llvm adcb87948, clang 15.0.4,
# target linx64v5-unknown-linux-musl) — no toolchain needed to reproduce.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GFSIM=${GFSIM:-./bin/gfsim}
GFRUN=${GFRUN:-./bin/gfrun}
run() { echo; echo "=== $1 ==="; shift; timeout 180 "$@" 2>&1 | tail -4; echo "(exit=$?)"; }

echo "######## A. functional model gfrun (all four ELFs must PASS, R2 = 0) ########"
for e in barrier_repro nobarrier_repro moe_dispatch_mt moe_combine_mt; do
    run "gfrun 4-PE: $e" "$GFRUN" -t 1 -s softcore.multiThreadNum=4 -f "$HERE/elf/$e.elf"
done

echo
echo "######## B. timing model gfsim, SMT4 (all four FAIL — this is the bug) ########"
echo "# B1. pure barrier, no tile ops: 4 threads spin forever, no progress"
run "gfsim 4T barrier_repro" "$GFSIM" -f "$HERE/elf/barrier_repro.elf" -s core.threadCount=4 core.vec_core_num=4
echo "#    (expected: rc=124 timeout; log tail shows all 4 tids retired ~300k blocks"
echo "#     all spinning at BPC 0x1131e — flag writes never become visible cross-PE)"

echo "# B2. control, no cross-PE polling: leader deadlock (park/exit semantics)"
run "gfsim 4T nobarrier_repro" "$GFSIM" -f "$HERE/elf/nobarrier_repro.elf" -s core.threadCount=4 core.vec_core_num=4
echo "#    (expected: rc=134, Deadlock detected at thread 0)"

echo "# B3. moe_dispatch_mt (real 4-PE kernel): RAS assertion at commit"
run "gfsim 4T moe_dispatch_mt" "$GFSIM" -f "$HERE/elf/moe_dispatch_mt.elf" -s core.threadCount=4 core.vec_core_num=4
echo "#    (expected: rc=134, bfu_ras.cpp:172 RAS::RunAtCommit 'spec table valid'"
echo "#     should be true at commit!)"

echo "# B4. moe_combine_mt (real 4-PE kernel): RAS assertion at restore"
run "gfsim 4T moe_combine_mt" "$GFSIM" -f "$HERE/elf/moe_combine_mt.elf" -s core.threadCount=4 core.vec_core_num=4
echo "#    (expected: rc=134, bfu_ras.cpp:39 RAS::restore 'spec_table[spec_wptr].vld')"

echo
echo "######## C. config gating (how to even reach SMT4 on gfsim) ########"
echo "# C1. threadCount=4 without vec_core_num=4 -> fail-fast assertion"
run "gfsim threadCount=4 only" "$GFSIM" -f "$HERE/elf/moe_dispatch_mt.elf" -s core.threadCount=4
echo "#    (expected: rc=134, BIssue.cpp:3138 'peid < blockDispatchQ[biq.type].size()')"
echo "# C2. default (1 thread) -> PE0 spins forever on flags PE1-3 never set"
run "gfsim default 1T" "$GFSIM" -f "$HERE/elf/moe_dispatch_mt.elf"
echo "#    (expected: rc=124, tid:0 spinning at BPC 0x1195a)"
