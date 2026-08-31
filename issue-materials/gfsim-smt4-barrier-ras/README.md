# gfsim SMT4 materials — 4-PE SPMD barrier kernels cannot be simulated

Companion materials for LinxISA/SuperScalarModel issue "[gfsim][GROUP] ...".
Operator source of the two real kernels: PTO-ISA/SuperNPUBench PR #95.

## Layout

```
repro.sh                 one-click: gfrun matrix (4x PASS) + gfsim matrix (4x FAIL) + config gating
src/
  barrier_repro.cpp      minimal cross-PE barrier repro — NO tile ops, NO MoE logic (~50 LOC)
  nobarrier_repro.cpp    control: same shape, local delay instead of cross-PE spin
  kernels/               the two real 4-PE SPMD kernels (same as PR #95)
  tests/                 their test mains (PE0-only verification + non-leader park)
elf/                     4 prebuilt ELFs (TileOP f94bc12 x llvm adcb87948, clang 15.0.4)
logs/                    collected evidence (trimmed; gfrun full-run logs summarized)
```

## Expected results (verified at SuperScalarModel d8903938, 0828 workspace build)

| ELF | gfrun 4-PE | gfsim threadCount=4 vec_core_num=4 |
|---|---|---|
| barrier_repro (no tile ops at all) | PASS R2=0 | **rc=124** 4 threads spin forever at BPC 0x1131e (~300k retired blocks, zero progress) |
| nobarrier_repro (control, no cross-PE polling) | PASS R2=0 | **rc=134** Deadlock at thread 0 (leader done, workers parked — no termination path) |
| moe_dispatch_mt (real kernel) | PASS R2=0 | **rc=134** bfu_ras.cpp:172 RAS::RunAtCommit assertion `e.vld` |
| moe_combine_mt (real kernel) | PASS R2=0 | **rc=134** bfu_ras.cpp:39 RAS::restore assertion `spec_table[spec_wptr].vld` |

Config gating on the way to SMT4 (logs C1/C2):
- `core.threadCount=4` alone → BIssue.cpp:3138 fail-fast (`vec_core_num` must be >= threadCount)
- default 1 thread → PE0 spins on flags PE1-3 never set (barrier needs all 4 PEs)

## Key claim (not an operator problem)

`barrier_repro` contains zero tile instructions and zero operator logic — only
`get_thread_idx()`, volatile flag stores/loads and ALU. It passes gfrun (R2=0)
and hangs gfsim SMT4. The two real kernels are additionally bit-verified on
gfrun (RNE bf16 reference, fault-injection returns non-zero R2). Therefore the
gfsim failures are timing-model defects, independent of operator semantics.

## Rebuild from source (optional, needs linx toolchain)

```bash
export COMPILER_DIR=/path/to/linx_blockisa_llvm_musl/bin
# barrier repros: drop src/*.cpp into any multi_thread test dir and build via
# its Makefile (TARGET name is fixed; see test/kernel/multi_thread/* pattern)
# real kernels: PTO-ISA/SuperNPUBench PR #95 (kernels/moe_{dispatch,combine}_mt)
```
