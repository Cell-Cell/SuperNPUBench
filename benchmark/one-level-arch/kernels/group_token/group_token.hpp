#ifndef GROUP_TOKEN_HPP
#define GROUP_TOKEN_HPP

#include <common/pto_tileop.hpp>
#include <cstdint>

// ============================================================================
// MoE Token Grouping operator (PTO one-level-arch)
//
// Implements the core of MoE (Mixture of Experts) dispatch: token grouping.
// Two phases, mirroring the cann-samples group_token_story reference:
//
//   Phase 1 — CalTokenPerExpertCnt
//     topkIndex [bs * k]  ->  tokenPerExpertCnt [expertNum]
//     Histogram: count how many tokens select each expert.
//
//   Phase 2 — GroupToken
//     topkIndex [bs * k]  ->  groupedTokenIds [expertPerRank * bs]
//                              expertSectionTokenCnt [expertPerRank]
//     For each token, find the minimum "local expert id" (expertId %
//     expertPerRank) among its top-k experts, then scatter the token id
//     into the corresponding expert section via a per-section write pointer.
//
// The scalar implementations below are unguarded and compile under every
// backend (__linx / __cpu_sim__ / __ARM_FEATURE_SME), guaranteeing correct
// results on gfrun/gfsim.  An optional SIMT-accelerated histogram is provided
// under #ifndef __linx, following the same convention as sort/topk.hpp.
// ============================================================================

// --- MoE topology constants (matching gen_expert_ids.py defaults) ---
constexpr uint32_t kBS            = 512;
constexpr uint32_t kTopK          = 16;
constexpr uint32_t kExpertPerRank = 4;
constexpr uint32_t kRankPerPod    = 16;
constexpr uint32_t kSuperPodNum   = 2;
constexpr uint32_t kExpertPerPod  = kExpertPerRank * kRankPerPod;   // 64
constexpr uint32_t kExpertNum     = kExpertPerPod * kSuperPodNum;   // 128
constexpr uint32_t kTopKEleNum    = kBS * kTopK;                     // 8192

// --- Tile type: 16×16 uint32 = 256 elements, fits kExpertNum=128 with padding
using TileU32 = Tile<Location::Vec, uint32_t, 16, 16, BLayout::RowMajor>;

// ============================================================================
// Phase 1 (scalar): CalTokenPerExpertCnt
//   Count occurrences of each expert id in topkIndex.
//   Works under all backends — the primary path used by gfrun/gfsim.
// ============================================================================
static inline void calTokenPerExpertCnt_scalar(const uint32_t *topkIndex,
                                                uint32_t *tokenPerExpertCnt,
                                                uint32_t expertNum,
                                                uint32_t topkEleNum)
{
    for (uint32_t i = 0; i < expertNum; i++) {
        tokenPerExpertCnt[i] = 0;
    }
    for (uint32_t i = 0; i < topkEleNum; i++) {
        uint32_t expertId = topkIndex[i];
        if (expertId < expertNum) {
            tokenPerExpertCnt[expertId]++;
        }
    }
}

#if !defined(__linx) && !defined(__cpu_sim__)  // SIMT <<<>>> launch needs linx/SME compiler

// ============================================================================
// Phase 1 (SIMT): CalTokenPerExpertCnt
//   Grid: <<<1, expertNum, 1>>>  (one lane per expert)
//   Each lane scans the entire topkIndex and counts matches for its expert id.
//   Writes count to dst[lane_id].
// ============================================================================
template <typename tile_shape_out>
void __vec__ CalTokenPerExpertCnt_Vec_Impl(
    typename tile_shape_out::TileDType __out__ dst,
    const uint32_t* __in__ src,
    uint32_t topkEleNum)
{
    size_t expertId = blkv_get_index_y();
    uint32_t count = 0;
    for (uint32_t i = 0; i < topkEleNum; i++) {
        if (src[i] == expertId) {
            count++;
        }
    }
    blkv_get_tile_ptr(dst)[expertId] = count;
}

template <typename tile_shape_out>
void CalTokenPerExpertCnt_Impl(tile_shape_out& dst, const uint32_t* src,
                               uint32_t topkEleNum, uint32_t expertNum)
{
    (void)expertNum;
    CalTokenPerExpertCnt_Vec_Impl<tile_shape_out>
        <<<1, expertNum, 1>>>(dst.data(), src, topkEleNum);
}

#endif // !__linx && !__cpu_sim__

// ============================================================================
// Phase 2 (scalar): GroupToken
//   For each token, find min local expert id, scatter token id into section.
//   Works under all backends — the primary path used by gfrun/gfsim.
// ============================================================================
static inline void groupToken_scalar(const uint32_t *topkIndex,
                                      uint32_t *groupedTokenIds,
                                      uint32_t *expertSectionTokenCnt,
                                      uint32_t batchSize,
                                      uint32_t topk,
                                      uint32_t expertPerRank)
{
    for (uint32_t i = 0; i < expertPerRank; i++) {
        expertSectionTokenCnt[i] = 0;
    }
    for (uint32_t i = 0; i < batchSize; i++) {
        uint32_t minLocalExpId = expertPerRank;
        uint32_t stop = (i + 1) * topk;
        for (uint32_t j = i * topk; j < stop; j++) {
            uint32_t curLocalExpId = topkIndex[j] % expertPerRank;
            if (curLocalExpId < minLocalExpId) {
                minLocalExpId = curLocalExpId;
            }
        }
        uint32_t idxInSection = expertSectionTokenCnt[minLocalExpId]++;
        groupedTokenIds[minLocalExpId * batchSize + idxInSection] = i;
    }
}

// ============================================================================
// Convenience entry point: run both phases.
// ============================================================================
static inline void runGroupToken(const uint32_t *topkIndex,
                                  uint32_t *tokenPerExpertCnt,
                                  uint32_t *groupedTokenIds,
                                  uint32_t *expertSectionTokenCnt)
{
    calTokenPerExpertCnt_scalar(topkIndex, tokenPerExpertCnt,
                                 kExpertNum, kTopKEleNum);
    groupToken_scalar(topkIndex, groupedTokenIds, expertSectionTokenCnt,
                       kBS, kTopK, kExpertPerRank);
}

#endif // GROUP_TOKEN_HPP
