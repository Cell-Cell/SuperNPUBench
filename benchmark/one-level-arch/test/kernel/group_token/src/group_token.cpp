#include <common/pto_tileop.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "benchmark.h"
#include "group_token/group_token.hpp"

static void genTopkIndex(uint32_t *topkIndex, uint32_t bs, uint32_t k,
                          uint32_t expertNum)
{
    uint32_t seed = 0x1234ABCDu;
    for (uint32_t i = 0; i < bs; i++) {
        for (uint32_t j = 0; j < k; j++) {
            seed = seed * 1103515245u + 12345u;
            topkIndex[i * k + j] = (seed >> 16) % expertNum;
        }
    }
}

int main()
{
#ifndef __linx
    printf("=== Group Token Test (MoE Token Grouping) ===\n");
    printf("BS=%u  TopK=%u  ExpertPerRank=%u  ExpertNum=%u\n",
           kBS, kTopK, kExpertPerRank, kExpertNum);
    fflush(stdout);
#endif

    uint32_t topkIndex[kTopKEleNum];
    uint32_t tokenPerExpertCnt[kExpertNum];
    uint32_t groupedTokenIds[kExpertPerRank * kBS];
    uint32_t expertSectionTokenCnt[kExpertPerRank];

    genTopkIndex(topkIndex, kBS, kTopK, kExpertNum);

    for (uint32_t i = 0; i < kExpertNum; i++) tokenPerExpertCnt[i] = 0;
    for (uint32_t i = 0; i < kExpertPerRank * kBS; i++) groupedTokenIds[i] = 0;
    for (uint32_t i = 0; i < kExpertPerRank; i++) expertSectionTokenCnt[i] = 0;

    BENCHSTART;
    runGroupToken(topkIndex, tokenPerExpertCnt,
                  groupedTokenIds, expertSectionTokenCnt);
    BENCHEND;

    uint32_t totalGrouped = 0;
    for (uint32_t i = 0; i < kExpertPerRank; i++) {
        totalGrouped += expertSectionTokenCnt[i];
    }

    int ret = (totalGrouped == kBS) ? 0 : 1;

#ifndef __linx
    printf("Total tokens grouped: %u (expected %u)\n", totalGrouped, kBS);
    printf("Section counts: ");
    for (uint32_t i = 0; i < kExpertPerRank; i++)
        printf("%u ", expertSectionTokenCnt[i]);
    printf("\n");
    printf("Expert counts: ");
    uint32_t totalCnt = 0;
    for (uint32_t i = 0; i < kExpertNum; i++) totalCnt += tokenPerExpertCnt[i];
    printf("total=%u (expected %u)\n", totalCnt, kTopKEleNum);
    printf("%s\n", ret ? "FAIL" : "PASS");
    fflush(stdout);
#endif
    return ret;
}
