#include <common/pto_tileop.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "benchmark.h"
#include "group_token_old/group_token_old.hpp"

// ============================================================================
// genTopkIndex — 生成测试用的 topkIndex 输入数据
//
// 功能：使用 LCG（线性同余生成器）伪随机算法生成 topkIndex 数组，
//       模拟 MoE routing 的输出（每个 token 选中的 top-k expert id）。
//       对应 .asc 中从文件读取 topk_idx 的数据源，这里用随机生成替代。
//
// 输入：bs        — token 总数（512）
//       k         — 每个 token 选中的 expert 数（16）
//       expertNum — expert 总数（128），生成的 expertId ∈ [0, expertNum)
// 输出：topkIndex — [bs*k] 生成的 top-k expert id 列表（共 8192 个元素）
//
// 算法：seed = seed * 1103515245 + 12345（glibc LCG 参数），
//       expertId = (seed >> 16) % expertNum
// ============================================================================
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

// ============================================================================
// 标量参考实现（独立于算子，用于验证算子输出的正确性）
// 对应 .asc 中 host 侧的 refCalTokenPerExpertCnt / refGroupToken / refSortByLocalExpId。
// ============================================================================

// ============================================================================
// refCalTokenPerExpertCnt — 参考实现：Phase 1 直方图
//
// 功能：与 calTokenPerExpertCnt_scalar 完全相同的逻辑，作为独立参考用于验证。
//       遍历 topkIndex，统计每个 expert 被多少个 token 选中。
//
// 输入：topkIndex   — [topkEleNum] top-k expert id 列表
//       expertNum   — expert 总数
//       topkEleNum  — topkIndex 元素总数
// 输出：refExpertCnt — [expertNum] 每个 expert 的参考计数
// ============================================================================
static void refCalTokenPerExpertCnt(const uint32_t *topkIndex,
                                     uint32_t *refExpertCnt,
                                     uint32_t expertNum, uint32_t topkEleNum)
{
    for (uint32_t i = 0; i < expertNum; i++) refExpertCnt[i] = 0;
    for (uint32_t i = 0; i < topkEleNum; i++) {
        if (topkIndex[i] < expertNum) refExpertCnt[topkIndex[i]]++;
    }
}

// ============================================================================
// refGroupToken — 参考实现：Phase 2 scatter
//
// 功能：与 groupToken_scalar 相同的 scatter 逻辑，作为独立参考用于验证。
//       对每个 token 计算 minLocalExpId，按 section scatter token id。
//       不含 pod info 计算（验证 pod info 在 main 中单独做）。
//
// 输入：topkIndex       — [bs*k] top-k expert id 列表
//       bs              — token 总数
//       topk            — 每个 token 选中的 expert 数
//       expertPerRank   — 每个 rank 的本地 expert 数（分区数）
// 输出：refGroupedIds   — [expertPerRank*bs] scatter 分组后的参考 token id
//       refSectionCnt   — [expertPerRank] 各分区参考 token 数
// ============================================================================
static void refGroupToken(const uint32_t *topkIndex,
                           uint32_t *refGroupedIds,
                           uint32_t *refSectionCnt,
                           uint32_t bs, uint32_t topk, uint32_t expertPerRank)
{
    for (uint32_t i = 0; i < expertPerRank; i++) refSectionCnt[i] = 0;
    for (uint32_t i = 0; i < bs; i++) {
        uint32_t minLocal = expertPerRank;
        for (uint32_t j = 0; j < topk; j++) {
            uint32_t local = topkIndex[i * topk + j] % expertPerRank;
            if (local < minLocal) minLocal = local;
        }
        uint32_t idx = refSectionCnt[minLocal]++;
        refGroupedIds[minLocal * bs + idx] = i;
    }
}

// ============================================================================
// refSortByLocalExpId — 参考实现：Phase 3 FloorFunc + counting sort
//
// 功能：与 floorFunc_scalar + sortByLocalExpId_scalar 相同的排序逻辑，
//       作为独立参考用于验证。先计算每个 token 的 minLocalExpId，
//       再做 counting sort 生成连续分区的 token id 列表。
//
// 输入：topkIndex         — [bs*k] top-k expert id 列表
//       bs                — token 总数
//       topk              — 每个 token 选中的 expert 数
//       expertPerRank     — 每个 rank 的本地 expert 数（排序桶数）
// 输出：refSortedIds      — [bs] 参考排序后的 token id 列表
//       refSectionStarts  — [expertPerRank+1] 参考各分区起始边界
// ============================================================================
static void refSortByLocalExpId(const uint32_t *topkIndex,
                                 uint32_t *refSortedIds,
                                 uint32_t *refSectionStarts,
                                 uint32_t bs, uint32_t topk,
                                 uint32_t expertPerRank)
{
    static uint32_t minLocalExpIds[kBS];
    for (uint32_t i = 0; i < bs; i++) {
        uint32_t minLocal = expertPerRank;
        for (uint32_t j = 0; j < topk; j++) {
            uint32_t local = topkIndex[i * topk + j] % expertPerRank;
            if (local < minLocal) minLocal = local;
        }
        minLocalExpIds[i] = minLocal;
    }
    uint32_t counts[kExpertPerRank];
    for (uint32_t i = 0; i < expertPerRank; i++) counts[i] = 0;
    for (uint32_t i = 0; i < bs; i++) counts[minLocalExpIds[i]]++;
    refSectionStarts[0] = 0;
    for (uint32_t i = 0; i < expertPerRank; i++)
        refSectionStarts[i + 1] = refSectionStarts[i] + counts[i];
    uint32_t writePos[kExpertPerRank];
    for (uint32_t i = 0; i < expertPerRank; i++)
        writePos[i] = refSectionStarts[i];
    for (uint32_t i = 0; i < bs; i++) {
        uint32_t section = minLocalExpIds[i];
        refSortedIds[writePos[section]++] = i;
    }
}

// ============================================================================
// main — 测试驱动入口
//
// 功能：生成测试数据 → 调用算子（runGroupTokenOld）→ 计算参考结果 → 逐项验证 → 返回结果。
//       对应 .asc 中 RunGroupToken() 的 host 侧验证流程。
//
// 执行流程：
//   1. 声明输入/输出数组（static 移入 .bss，避免栈溢出）
//   2. genTopkIndex 生成随机 topkIndex 输入数据
//   3. 初始化所有输出数组为 0
//   4. BENCHSTART → runGroupTokenOld → BENCHEND（执行算子）
//   5. 计算标量参考结果（refCalTokenPerExpertCnt / refGroupToken / refSortByLocalExpId）
//   6. 逐项验证：
//      - Phase 1: expert counts（128 个 expert 计数是否匹配）
//      - Phase 2: section counts（4 个分区计数是否匹配）
//      - Phase 2: grouped ids（各分区 token id 集合是否匹配，排序后比较）
//      - Phase 2: pod info（tokenSuperPodInfo 是否匹配）
//      - Phase 3: section bounds（分区边界是否匹配）
//      - Phase 3: sorted ids（排序后 token id 逐元素精确匹配）
//   7. 返回 R2：0=PASS，1/2/3/4/5=对应 Phase 验证失败
//
// 返回值（R2 寄存器）：
//   0 — 全部 PASS
//   1 — Phase 1 (expert counts) 失败
//   2 — Phase 2 (section counts) 失败
//   3 — Phase 2 (grouped ids) 失败
//   4 — Phase 3 (section bounds) 失败
//   5 — Phase 3 (sorted ids) 失败
//
// 注意：__linx 下不打印（无 printf），通过 R2 返回值判断结果。
// ============================================================================
int main()
{
#ifndef __linx
    printf("=== Group Token Old Test (3-phase: histogram + scatter + sort) ===\n");
    printf("BS=%u  TopK=%u  ExpertPerRank=%u  ExpertNum=%u\n",
           kBS, kTopK, kExpertPerRank, kExpertNum);
    fflush(stdout);
#endif

    static uint32_t topkIndex[kTopKEleNum];
    static uint32_t tokenPerExpertCnt[kExpertNum];
    static uint32_t groupedTokenIds[kExpertPerRank * kBS];
    static uint32_t tokenSuperPodInfo[kExpertPerRank * kBS * kSuperPodNum];
    static uint32_t expertSectionTokenCnt[kExpertPerRank];
    static uint32_t sortedTokenIds[kBS];
    static uint32_t sectionStarts[kExpertPerRank + 1];

    genTopkIndex(topkIndex, kBS, kTopK, kExpertNum);

    for (uint32_t i = 0; i < kExpertNum; i++) tokenPerExpertCnt[i] = 0;
    for (uint32_t i = 0; i < kExpertPerRank * kBS; i++) groupedTokenIds[i] = 0;
    for (uint32_t i = 0; i < kExpertPerRank * kBS * kSuperPodNum; i++) tokenSuperPodInfo[i] = 0;
    for (uint32_t i = 0; i < kExpertPerRank; i++) expertSectionTokenCnt[i] = 0;
    for (uint32_t i = 0; i < kBS; i++) sortedTokenIds[i] = 0;

    BENCHSTART;
    runGroupTokenOld(topkIndex, tokenPerExpertCnt,
                      groupedTokenIds, tokenSuperPodInfo, expertSectionTokenCnt,
                      sortedTokenIds, sectionStarts);
    BENCHEND;

    // --- compute reference results ---
    static uint32_t refExpertCnt[kExpertNum];
    static uint32_t refGroupedIds[kExpertPerRank * kBS];
    static uint32_t refSectionCnt[kExpertPerRank];
    static uint32_t refSortedIds[kBS];
    static uint32_t refSectionStarts[kExpertPerRank + 1];

    for (uint32_t i = 0; i < kExpertPerRank * kBS; i++) refGroupedIds[i] = 0;
    refCalTokenPerExpertCnt(topkIndex, refExpertCnt, kExpertNum, kTopKEleNum);
    refGroupToken(topkIndex, refGroupedIds, refSectionCnt,
                   kBS, kTopK, kExpertPerRank);
    refSortByLocalExpId(topkIndex, refSortedIds, refSectionStarts,
                         kBS, kTopK, kExpertPerRank);

    // --- verify Phase 1: expert counts ---
    int cntMatch = 0;
    for (uint32_t i = 0; i < kExpertNum; i++) {
        if (tokenPerExpertCnt[i] == refExpertCnt[i]) cntMatch++;
    }

    // --- verify Phase 2: section counts ---
    int secMatch = 0;
    for (uint32_t i = 0; i < kExpertPerRank; i++) {
        if (expertSectionTokenCnt[i] == refSectionCnt[i]) secMatch++;
    }

    // --- verify Phase 2: grouped token ids (as sorted sets per section) ---
    int idMatch = 0;
    int idTotal = 0;
    for (uint32_t s = 0; s < kExpertPerRank; s++) {
        uint32_t n = expertSectionTokenCnt[s];
        idTotal += n;
        static uint32_t buf[kBS];
        static uint32_t ref[kBS];
        for (uint32_t i = 0; i < n; i++) {
            buf[i] = groupedTokenIds[s * kBS + i];
            ref[i] = refGroupedIds[s * kBS + i];
        }
        for (uint32_t i = 0; i < n; i++) {
            for (uint32_t j = i + 1; j < n; j++) {
                if (buf[i] > buf[j]) { uint32_t t = buf[i]; buf[i] = buf[j]; buf[j] = t; }
                if (ref[i] > ref[j]) { uint32_t t = ref[i]; ref[i] = ref[j]; ref[j] = t; }
            }
        }
        for (uint32_t i = 0; i < n; i++) {
            if (buf[i] == ref[i]) idMatch++;
        }
    }

    // --- verify Phase 2: tokenSuperPodInfo (paired with groupedTokenIds) ---
    int podInfoMatch = 0;
    int podInfoTotal = 0;
    for (uint32_t s = 0; s < kExpertPerRank; s++) {
        uint32_t n = refSectionCnt[s];
        podInfoTotal += n * kSuperPodNum;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t tokenId = groupedTokenIds[s * kBS + i];
            uint32_t expectedPod[kSuperPodNum];
            for (uint32_t j = 0; j < kSuperPodNum; j++) expectedPod[j] = 0;
            for (uint32_t j = 0; j < kTopK; j++) {
                uint32_t expertId = topkIndex[tokenId * kTopK + j];
                uint32_t curDstPod = expertId / kExpertPerPod;
                if (curDstPod < kSuperPodNum) expectedPod[curDstPod] = 1;
            }
            uint32_t offset = s * kBS * kSuperPodNum + i * kSuperPodNum;
            for (uint32_t j = 0; j < kSuperPodNum; j++) {
                if (tokenSuperPodInfo[offset + j] == expectedPod[j]) podInfoMatch++;
            }
        }
    }

    // --- verify Phase 3: section boundaries ---
    int boundMatch = 0;
    for (uint32_t i = 0; i <= kExpertPerRank; i++) {
        if (sectionStarts[i] == refSectionStarts[i]) boundMatch++;
    }

    // --- verify Phase 3: sorted token ids (exact match, order matters) ---
    int sortMatch = 0;
    for (uint32_t i = 0; i < kBS; i++) {
        if (sortedTokenIds[i] == refSortedIds[i]) sortMatch++;
    }

    int ret = 0;
    if (cntMatch != (int)kExpertNum) ret = 1;
    else if (secMatch != (int)kExpertPerRank) ret = 2;
    else if (idMatch != idTotal) ret = 3;
    else if (podInfoMatch != podInfoTotal) {
        uint32_t s0 = 0, i0 = 0;
        uint32_t tokenId0 = groupedTokenIds[s0 * kBS + i0];
        uint32_t expectedPod0 = 0;
        for (uint32_t j = 0; j < kTopK; j++) {
            uint32_t expertId = topkIndex[tokenId0 * kTopK + j];
            uint32_t curDstPod = expertId / kExpertPerPod;
            if (curDstPod < kSuperPodNum) expectedPod0 = 1;
        }
        uint32_t offset0 = s0 * kBS * kSuperPodNum + i0 * kSuperPodNum;
        uint32_t actualVal = tokenSuperPodInfo[offset0];
        ret = actualVal * 10000 + expectedPod0 * 1000 + tokenId0;
    }
    else if (boundMatch != (int)(kExpertPerRank + 1)) ret = 4;
    else if (sortMatch != (int)kBS) ret = 5;

#ifndef __linx
    printf("\n=== Verification (vs scalar reference) ===\n");
    printf("Phase 1 (expert counts):   %d/%u match\n", cntMatch, kExpertNum);
    printf("Phase 2 (section counts):  %d/%u match\n", secMatch, kExpertPerRank);
    printf("Phase 2 (grouped ids):     %d/%d match\n", idMatch, idTotal);
    printf("Phase 2 (pod info):        %d/%d match\n", podInfoMatch, podInfoTotal);
    printf("Phase 3 (section bounds):  %d/%u match\n", boundMatch, kExpertPerRank + 1);
    printf("Phase 3 (sorted ids):      %d/%u match\n", sortMatch, kBS);
    printf("\nSection counts: ");
    for (uint32_t i = 0; i < kExpertPerRank; i++)
        printf("%u ", expertSectionTokenCnt[i]);
    printf("\nSection starts: ");
    for (uint32_t i = 0; i <= kExpertPerRank; i++)
        printf("%u ", sectionStarts[i]);
    printf("\n");
    printf("%s\n", ret ? "FAIL" : "PASS");
    fflush(stdout);
#endif
    return ret;
}
