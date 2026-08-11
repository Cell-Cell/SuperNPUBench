#include <common/pto_tileop.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "benchmark.h"

int main() {
    static uint32_t input_data[16];
    static uint32_t fail_count;

    input_data[0] = 57;
    input_data[1] = 70;
    input_data[2] = 100;
    input_data[3] = 127;
    input_data[4] = 0;
    input_data[5] = 3;
    input_data[6] = 64;
    input_data[7] = 128;
    input_data[8] = 63;
    input_data[9] = 65;
    input_data[10] = 126;
    input_data[11] = 1;
    input_data[12] = 2;
    input_data[13] = 124;
    input_data[14] = 125;
    input_data[15] = 255;

    fail_count = 0;

    for (uint32_t i = 0; i < 16; i++) {
        uint32_t val = input_data[i];

        uint32_t mod_result = val % 4;
        uint32_t div_result = val / 64;
        uint32_t shift_result = val >> 6;

        uint32_t expected_mod = 0;
        uint32_t tmp = val;
        while (tmp >= 4) { tmp -= 4; }
        expected_mod = tmp;

        uint32_t expected_div = 0;
        tmp = val;
        while (tmp >= 64) { tmp -= 64; expected_div++; }

        if (mod_result != expected_mod) fail_count++;
        if (div_result != expected_div) fail_count += 100;
        if (shift_result != expected_div) fail_count += 10000;
    }

    return fail_count;
}
