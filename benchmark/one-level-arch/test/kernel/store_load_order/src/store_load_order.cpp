/*
 * store_load_order.cpp — 复现 gfrun store/load 顺序问题的最小测试
 *
 * 不依赖 group_token_old 算子，仅模拟 "写后清零" 代码模式：
 *   output[i] = temp;   // store A: 把临时变量写入输出
 *   temp = 0;            // store B: 清零临时变量
 *
 * 如果 gfrun 先执行 store B 再执行 store A 的数据读取，
 * output[i] 会被写入 0 而非 temp 的实际值。
 *
 * 返回值 R2:
 *   0          = 全部通过（无 store/load 问题）
 *   1          = 基础模式失败（output 数组值错误）
 *   2          = volatile 模式也失败（问题更深）
 *   100+N      = 第 N 个用例失败（N 从 1 开始）
 */

#include <common/pto_tileop.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "benchmark.h"

/*
 * 测试 1: 基础写后清零模式（模拟 group_token_old 的 dstPodLocal）
 *
 * temp 是 2 元素数组，编译器可能优化到寄存器。
 * 每次迭代：设置 temp → 写入 output → 清零 temp。
 */
static void test_basic_pattern(uint32_t* output, uint32_t count) {
    uint32_t temp[2];
    temp[0] = 0;
    temp[1] = 0;

    for (uint32_t i = 0; i < count; i++) {
        /* 模拟遍历 expert 设置 pod 标志 */
        temp[0] = 1;  /* pod 0 有 expert */
        temp[1] = 1;  /* pod 1 有 expert */

        /* 写入 output 然后清零 temp（写后清零模式） */
        output[i * 2 + 0] = temp[0];
        output[i * 2 + 1] = temp[1];
        temp[0] = 0;
        temp[1] = 0;
    }
}

/*
 * 测试 2: volatile 版本（阻止编译器优化到寄存器）
 *
 * 如果 volatile 版本通过但基础版本失败，
 * 说明根因是编译器对 temp 的寄存器优化。
 */
static void test_volatile_pattern(uint32_t* output, uint32_t count) {
    volatile uint32_t temp[2];
    temp[0] = 0;
    temp[1] = 0;

    for (uint32_t i = 0; i < count; i++) {
        temp[0] = 1;
        temp[1] = 1;

        output[i * 2 + 0] = temp[0];
        output[i * 2 + 1] = temp[1];
        temp[0] = 0;
        temp[1] = 0;
    }
}

/*
 * 测试 3: 清零移到循环开头（改变 store 顺序）
 *
 * 如果这个版本也失败，说明不是简单的 store 指令顺序问题。
 */
static void test_clear_first_pattern(uint32_t* output, uint32_t count) {
    uint32_t temp[2];

    for (uint32_t i = 0; i < count; i++) {
        temp[0] = 0;
        temp[1] = 0;

        temp[0] = 1;
        temp[1] = 1;

        output[i * 2 + 0] = temp[0];
        output[i * 2 + 1] = temp[1];
    }
}

/*
 * 测试 4: 间接索引（模拟 group_token_old 的 dstPodLocal[curDstPod] = 1）
 *
 * 通过运行时索引写入 temp，更接近原始代码模式。
 */
static void test_indexed_pattern(uint32_t* output, uint32_t* indices, uint32_t count) {
    uint32_t temp[2];
    temp[0] = 0;
    temp[1] = 0;

    for (uint32_t i = 0; i < count; i++) {
        /* 用运行时索引设置 temp（模拟 curDstPod 计算） */
        temp[indices[i * 2 + 0]] = 1;
        temp[indices[i * 2 + 1]] = 1;

        output[i * 2 + 0] = temp[0];
        output[i * 2 + 1] = temp[1];
        temp[0] = 0;
        temp[1] = 0;
    }
}

int main() {
    static uint32_t output[512 * 2];
    static uint32_t indices[512 * 2];
    uint32_t count = 256;

    /* 初始化 indices：交替 0,1 */
    for (uint32_t i = 0; i < count; i++) {
        indices[i * 2 + 0] = 0;
        indices[i * 2 + 1] = 1;
    }

    uint32_t ret = 0;

    /* 测试 1: 基础写后清零 */
    memset(output, 0xFF, sizeof(output));
    test_basic_pattern(output, count);
    {
        uint32_t fail = 0;
        for (uint32_t i = 0; i < count * 2; i++) {
            if (output[i] != 1) fail++;
        }
        if (fail > 0) {
            ret = 100 + 1;
        }
    }

    /* 测试 2: volatile 版本 */
    memset(output, 0xFF, sizeof(output));
    test_volatile_pattern(output, count);
    {
        uint32_t fail = 0;
        for (uint32_t i = 0; i < count * 2; i++) {
            if (output[i] != 1) fail++;
        }
        if (fail > 0 && ret == 0) {
            ret = 100 + 2;
        }
    }

    /* 测试 3: 清零移到开头 */
    memset(output, 0xFF, sizeof(output));
    test_clear_first_pattern(output, count);
    {
        uint32_t fail = 0;
        for (uint32_t i = 0; i < count * 2; i++) {
            if (output[i] != 1) fail++;
        }
        if (fail > 0 && ret == 0) {
            ret = 100 + 3;
        }
    }

    /* 测试 4: 间接索引模式 */
    memset(output, 0xFF, sizeof(output));
    test_indexed_pattern(output, indices, count);
    {
        uint32_t fail = 0;
        for (uint32_t i = 0; i < count * 2; i++) {
            if (output[i] != 1) fail++;
        }
        if (fail > 0 && ret == 0) {
            ret = 100 + 4;
        }
    }

    /* 编码失配数量用于诊断 */
    if (ret == 0) {
        return 0;
    } else {
        /* ret = 测试编号*10000 + 失配数 */
        uint32_t test_num = ret - 100;
        memset(output, 0xFF, sizeof(output));
        if (test_num == 1) test_basic_pattern(output, count);
        else if (test_num == 2) test_volatile_pattern(output, count);
        else if (test_num == 3) test_clear_first_pattern(output, count);
        else if (test_num == 4) test_indexed_pattern(output, indices, count);
        uint32_t fail = 0;
        for (uint32_t i = 0; i < count * 2; i++) {
            if (output[i] != 1) fail++;
        }
        return test_num * 10000 + fail;
    }
}
