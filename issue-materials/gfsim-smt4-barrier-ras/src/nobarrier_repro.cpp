// Control: identical program shape, but the cross-PE spin wait is replaced
// by a fixed local delay loop — no inter-PE volatile polling at all.
#include <common/pto_tileop.hpp>
#include <cstdint>

static volatile uint32_t sWorkSink = 0;

int main() {
    const uint32_t tid = get_thread_idx();

    volatile uint32_t sink = tid;
    for (uint32_t phase = 1; phase <= 3; ++phase) {
        for (uint32_t i = 0; i < 64; ++i) {
            sink = sink * 3u + i + 1u;
        }
        // local delay instead of cross-PE barrier
        for (uint32_t i = 0; i < 4096; ++i) {
            sink = sink + i;
        }
    }
    sWorkSink = sink;

    if (tid != 0) {
        for (;;) {
            sWorkSink = tid;
        }
    }
    return 0;
}
