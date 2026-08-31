// Minimal cross-PE barrier reproducer — NO tile ops, NO MoE logic.
// Exact same SPMD barrier idiom as moe_dispatch_mt / moe_combine_mt:
// volatile per-PE phase flags + spin wait + non-leader park loop.
#include <common/pto_tileop.hpp>
#include <cstdint>

static volatile uint32_t sPhaseDone[4];
static volatile uint32_t sWorkSink = 0;

static inline void reproBarrier(uint32_t phase)
{
    __asm__ volatile("" : : : "memory");
    sPhaseDone[get_thread_idx()] = phase;
    __asm__ volatile("" : : : "memory");
    for (uint32_t t = 0; t < 4; ++t) {
        while (sPhaseDone[t] < phase) {
        }
    }
    __asm__ volatile("" : : : "memory");
}

int main() {
    const uint32_t tid = get_thread_idx();

    // trivial per-PE "work" around each barrier (plain scalar ALU)
    volatile uint32_t sink = tid;
    for (uint32_t phase = 1; phase <= 3; ++phase) {
        for (uint32_t i = 0; i < 64; ++i) {
            sink = sink * 3u + i + 1u;
        }
        reproBarrier(phase);
    }
    sWorkSink = sink;

    // non-leader park (same convention as the MoE MT kernels)
    if (tid != 0) {
        for (;;) {
            sWorkSink = tid;
        }
    }
    return 0;
}
