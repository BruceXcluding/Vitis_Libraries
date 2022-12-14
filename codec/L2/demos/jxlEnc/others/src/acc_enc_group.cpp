// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "acc_enc_group.hpp"

#include <iomanip>
#include <iostream>
#include <utility>

#include "hwy/aligned_allocator.h"

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "xilinx/src/acc_enc_group.cpp"
#include <hwy/foreach_target.h>
#include <hwy/highway.h>

#include "lib/jxl/ac_strategy.h"
#include "lib/jxl/aux_out.h"
#include "lib/jxl/aux_out_fwd.h"
#include "lib/jxl/base/bits.h"
#include "lib/jxl/base/compiler_specific.h"
#include "lib/jxl/base/profiler.h"
#include "lib/jxl/common.h"
#include "lib/jxl/dct_util.h"
#include "lib/jxl/dec_transforms-inl.h"
#include "lib/jxl/enc_params.h"
#include "lib/jxl/enc_transforms-inl.h"
#include "lib/jxl/image.h"
#include "lib/jxl/quantizer-inl.h"
#include "lib/jxl/quantizer.h"
HWY_BEFORE_NAMESPACE();
namespace jxl {
namespace HWY_NAMESPACE {

// NOTE: caller takes care of extracting quant from rect of RawQuantField.
void QuantizeBlockAC(const Quantizer& quantizer,
                     const bool error_diffusion,
                     size_t c,
                     int32_t quant,
                     float qm_multiplier,
                     size_t quant_kind,
                     size_t xsize,
                     size_t ysize,
                     const float* JXL_RESTRICT block_in,
                     int32_t* JXL_RESTRICT block_out) {
    PROFILER_FUNC;
    const float* JXL_RESTRICT qm = quantizer.InvDequantMatrix(quant_kind, c);
    const float qac = quantizer.Scale() * quant;
    // Not SIMD-fied for now.
    float thres[4] = {0.5f, 0.6f, 0.6f, 0.65f};
    if (c != 1) {
        for (int i = 1; i < 4; ++i) {
            thres[i] = 0.75f;
        }
    }

    if (!error_diffusion) {
        HWY_CAPPED(float, kBlockDim) df;
        HWY_CAPPED(int32_t, kBlockDim) di;
        HWY_CAPPED(uint32_t, kBlockDim) du;
        const auto quant = Set(df, qac * qm_multiplier);

        for (size_t y = 0; y < ysize * kBlockDim; y++) {
            size_t yfix = static_cast<size_t>(y >= ysize * kBlockDim / 2) * 2;
            const size_t off = y * kBlockDim * xsize;
            for (size_t x = 0; x < xsize * kBlockDim; x += Lanes(df)) {
                auto thr = Zero(df);
                if (xsize == 1) {
                    HWY_ALIGN uint32_t kMask[kBlockDim] = {0, 0, 0, 0, ~0u, ~0u, ~0u, ~0u};
                    const auto mask = MaskFromVec(BitCast(df, Load(du, kMask + x)));
                    thr = IfThenElse(mask, Set(df, thres[yfix + 1]), Set(df, thres[yfix]));
                } else {
                    // Same for all lanes in the vector.
                    thr = Set(df, thres[yfix + static_cast<size_t>(x >= xsize * kBlockDim / 2)]);
                }

                const auto q = Load(df, qm + off + x) * quant;
                const auto in = Load(df, block_in + off + x);
                const auto val = q * in;
                const auto nzero_mask = Abs(val) >= thr;
                const auto v = ConvertTo(di, IfThenElseZero(nzero_mask, Round(val)));
                Store(v, di, block_out + off + x);
            }
        }
        return;
    }

retry:
    int hfNonZeros[4] = {};
    float hfError[4] = {};
    float hfMaxError[4] = {};
    size_t hfMaxErrorIx[4] = {};
    for (size_t y = 0; y < ysize * kBlockDim; y++) {
        for (size_t x = 0; x < xsize * kBlockDim; x++) {
            const size_t pos = y * kBlockDim * xsize + x;
            if (x < xsize && y < ysize) {
                // Ensure block is initialized
                block_out[pos] = 0;
                continue;
            }
            const size_t hfix =
                (static_cast<size_t>(y >= ysize * kBlockDim / 2) * 2 + static_cast<size_t>(x >= xsize * kBlockDim / 2));
            const float val = block_in[pos] * (qm[pos] * qac * qm_multiplier);
            float v = (std::abs(val) < thres[hfix]) ? 0 : rintf(val);
            const float error = std::abs(val) - std::abs(v);
            hfError[hfix] += error;
            if (hfMaxError[hfix] < error) {
                hfMaxError[hfix] = error;
                hfMaxErrorIx[hfix] = pos;
            }
            if (v != 0.0f) {
                hfNonZeros[hfix] += std::abs(v);
            }
            block_out[pos] = static_cast<int32_t>(rintf(v));
        }
    }
    if (c != 1) return;
    // TODO(veluca): include AFV?
    const size_t kPartialBlockKinds = (1 << AcStrategy::Type::IDENTITY) | (1 << AcStrategy::Type::DCT2X2) |
                                      (1 << AcStrategy::Type::DCT4X4) | (1 << AcStrategy::Type::DCT4X8) |
                                      (1 << AcStrategy::Type::DCT8X4);
    if ((1 << quant_kind) & kPartialBlockKinds) return;
    float hfErrorLimit = 0.1f * (xsize * ysize) * kDCTBlockSize * 0.25f;
    bool goretry = false;
    for (int i = 1; i < 4; ++i) {
        if (hfError[i] >= hfErrorLimit && hfNonZeros[i] <= (xsize + ysize) * 0.25f) {
            if (thres[i] >= 0.4f) {
                thres[i] -= 0.01f;
                goretry = true;
            }
        }
    }
    if (goretry) goto retry;
    for (int i = 1; i < 4; ++i) {
        if (hfError[i] >= hfErrorLimit && hfNonZeros[i] == 0) {
            const size_t pos = hfMaxErrorIx[i];
            if (hfMaxError[i] >= 0.4f) {
                block_out[pos] = block_in[pos] > 0.0f ? 1.0f : -1.0f;
            }
        }
    }
}

// NOTE: caller takes care of extracting quant from rect of RawQuantField.
void QuantizeRoundtripYBlockAC(const Quantizer& quantizer,
                               const bool error_diffusion,
                               int32_t quant,
                               size_t quant_kind,
                               size_t xsize,
                               size_t ysize,
                               const float* JXL_RESTRICT biases,
                               float* JXL_RESTRICT inout,
                               int32_t* JXL_RESTRICT quantized) {
    QuantizeBlockAC(quantizer, error_diffusion, 1, quant, 1.0f, quant_kind, xsize, ysize, inout, quantized);

    PROFILER_ZONE("enc quant adjust bias");
    const float* JXL_RESTRICT dequant_matrix = quantizer.DequantMatrix(quant_kind, 1);

    HWY_CAPPED(float, kDCTBlockSize) df;
    HWY_CAPPED(int32_t, kDCTBlockSize) di;
    const auto inv_qac = Set(df, quantizer.inv_quant_ac(quant));
    for (size_t k = 0; k < kDCTBlockSize * xsize * ysize; k += Lanes(df)) {
        const auto quant = Load(di, quantized + k);
        const auto adj_quant = AdjustQuantBias(di, 1, quant, biases);
        const auto dequantm = Load(df, dequant_matrix + k);
        Store(adj_quant * dequantm * inv_qac, df, inout + k);
    }
}

void ComputeCoefficients(size_t group_idx,
                         PassesEncoderState* enc_state,
                         const Image3F& opsin,
                         Image3F* dc,

                         //==========acc interface========
                         size_t xsize,
                         size_t ysize,
                         std::vector<std::vector<float> >& dctIDT,
                         std::vector<std::vector<float> >& dct2x2,
                         std::vector<std::vector<float> >& dct4x4,
                         std::vector<std::vector<float> >& dct8x8,
                         std::vector<std::vector<float> >& dct16x16,
                         std::vector<std::vector<float> >& dct32x32,

                         std::vector<std::vector<float> >& dcIDT,
                         std::vector<std::vector<float> >& dc2x2,
                         std::vector<std::vector<float> >& dc4x4,
                         std::vector<std::vector<float> >& dc8x8,
                         std::vector<std::vector<float> >& dc16x16,
                         std::vector<std::vector<float> >& dc32x32
                         //================================
                         ) {
    PROFILER_FUNC;
    const Rect block_group_rect = enc_state->shared.BlockGroupRect(group_idx);
    const Rect group_rect = enc_state->shared.GroupRect(group_idx);
    const Rect cmap_rect(block_group_rect.x0() / kColorTileDimInBlocks, block_group_rect.y0() / kColorTileDimInBlocks,
                         DivCeil(block_group_rect.xsize(), kColorTileDimInBlocks),
                         DivCeil(block_group_rect.ysize(), kColorTileDimInBlocks));

    const size_t xsize_blocks = block_group_rect.xsize();
    const size_t ysize_blocks = block_group_rect.ysize();

    const size_t dc_stride = static_cast<size_t>(dc->PixelsPerRow());
    const size_t opsin_stride = static_cast<size_t>(opsin.PixelsPerRow());

    const ImageI& full_quant_field = enc_state->shared.raw_quant_field;
    const CompressParams& cparams = enc_state->cparams;

    // TODO(veluca): consider strategies to reduce this memory.
    auto mem = hwy::AllocateAligned<int32_t>(3 * AcStrategy::kMaxCoeffArea);
    auto fmem = hwy::AllocateAligned<float>(5 * AcStrategy::kMaxCoeffArea);
    float* JXL_RESTRICT scratch_space = fmem.get() + 3 * AcStrategy::kMaxCoeffArea;
    {
        // Only use error diffusion in Squirrel mode or slower.
        const bool error_diffusion = cparams.speed_tier <= SpeedTier::kSquirrel;
        constexpr HWY_CAPPED(float, kDCTBlockSize) d;

        int32_t* JXL_RESTRICT coeffs[kMaxNumPasses][3] = {};
        size_t num_passes = enc_state->progressive_splitter.GetNumPasses();
        JXL_DASSERT(num_passes > 0);
        for (size_t i = 0; i < num_passes; i++) {
            // TODO(veluca): 16-bit quantized coeffs are not implemented yet.
            JXL_ASSERT(enc_state->coeffs[i]->Type() == ACType::k32);
            for (size_t c = 0; c < 3; c++) {
                coeffs[i][c] = enc_state->coeffs[i]->PlaneRow(c, group_idx, 0).ptr32;
            }
        }

        HWY_ALIGN float* coeffs_in = fmem.get();
        HWY_ALIGN int32_t* quantized = mem.get();

        size_t offset = 0;

        for (size_t by = 0; by < ysize_blocks; ++by) {
            const int32_t* JXL_RESTRICT row_quant_ac = block_group_rect.ConstRow(full_quant_field, by);
            size_t ty = by / kColorTileDimInBlocks;
            const int8_t* JXL_RESTRICT row_cmap[3] = {
                cmap_rect.ConstRow(enc_state->shared.cmap.ytox_map, ty), nullptr,
                cmap_rect.ConstRow(enc_state->shared.cmap.ytob_map, ty),
            };
            const float* JXL_RESTRICT opsin_rows[3] = {
                group_rect.ConstPlaneRow(opsin, 0, by * kBlockDim), group_rect.ConstPlaneRow(opsin, 1, by * kBlockDim),
                group_rect.ConstPlaneRow(opsin, 2, by * kBlockDim),
            };
            float* JXL_RESTRICT dc_rows[3] = {
                block_group_rect.PlaneRow(dc, 0, by), block_group_rect.PlaneRow(dc, 1, by),
                block_group_rect.PlaneRow(dc, 2, by),
            };
            AcStrategyRow ac_strategy_row = enc_state->shared.ac_strategy.ConstRow(block_group_rect, by);
            for (size_t tx = 0; tx < DivCeil(xsize_blocks, kColorTileDimInBlocks); tx++) {
                const auto x_factor = Set(d, enc_state->shared.cmap.YtoXRatio(row_cmap[0][tx]));
                const auto b_factor = Set(d, enc_state->shared.cmap.YtoBRatio(row_cmap[2][tx]));
                for (size_t bx = tx * kColorTileDimInBlocks; bx < xsize_blocks && bx < (tx + 1) * kColorTileDimInBlocks;
                     ++bx) {
                    const AcStrategy acs = ac_strategy_row[bx];
                    if (!acs.IsFirstBlock()) continue;

                    size_t xblocks = acs.covered_blocks_x();
                    size_t yblocks = acs.covered_blocks_y();

                    CoefficientLayout(&yblocks, &xblocks); // QC: xblocks and yblocks are
                                                           // updated inside. Calculate
                                                           // how may horizontal 8x8
                                                           // blocks (xblocks) covered by
                                                           // the ACstrategy and vertical
                                                           // 8x8 blocks (yblocks)
                                                           // covered by the acs.

                    size_t size = kDCTBlockSize * xblocks * yblocks;

                    // DCT Y channel, roundtrip-quantize it and set DC.
                    const int32_t quant_ac = row_quant_ac[bx];
                    //          TransformFromPixels(acs.Strategy(), opsin_rows[1] + bx *
                    //          kBlockDim,
                    //                              opsin_stride, coeffs_in + size,
                    //                              scratch_space);

                    //         DCFromLowestFrequencies(acs.Strategy(), coeffs_in + size,
                    //                                 dc_rows[1] + bx, dc_stride);

                    size_t tile_xsize = (xsize + 63) / 64 * 64;
                    size_t tile_ysize = (ysize + 63) / 64 * 64;
                    float* coef_dct = coeffs_in + size;
                    size_t block_cnt8x8 = (block_group_rect.y0() + by) * (tile_xsize / 8) + block_group_rect.x0() + bx;
                    size_t block_cnt16x16 =
                        (block_group_rect.y0() + by) / 2 * (tile_xsize / 16) + (block_group_rect.x0() + bx) / 2;
                    size_t block_cnt32x32 =
                        (block_group_rect.y0() + by) / 4 * (tile_xsize / 32) + (block_group_rect.x0() + bx) / 4;

#ifdef XLNX_QC_DEBUG_ENC_GROUP
                    if (acs.RawStrategy() == 0) {
                        std::cout << "========================debug===================== "
                                     "convered blocks: "
                                  << acs.covered_blocks_x() << " tile_xsize: " << tile_xsize
                                  << " bx: " << block_group_rect.x0() << " " << bx << " by: " << block_group_rect.y0()
                                  << " " << by << std::endl;
                        for (int i = 0; i < 64; i++) {
                            std::cout << std::setw(15) << coef_dct[i] << " ";
                        }
                        std::cout << std::endl;
                        for (int i = 0; i < 64; i++) {
                            std::cout << std::setw(15) << dct8x8[1][64 * block_cnt8x8 + i] << " ";
                        }
                        std::cout << std::endl;
                        for (int i = 0; i < 64; i++) {
                            if (coef_dct[i] != dct8x8[1][64 * block_cnt8x8 + i]) std::cout << "!!!";
                        }
                        std::cout << std::endl;
                    }
#endif

                    for (int i = 0; i < 32 * 32; i++) {
                        if (acs.RawStrategy() == 0) {
                            if (i < 64) coef_dct[i] = dct8x8[1][64 * block_cnt8x8 + i];
                        } else if (acs.RawStrategy() == 1) {
                            if (i < 64) coef_dct[i] = dctIDT[1][64 * block_cnt8x8 + i];
                        } else if (acs.RawStrategy() == 2) {
                            if (i < 64) coef_dct[i] = dct2x2[1][64 * block_cnt8x8 + i];
                        } else if (acs.RawStrategy() == 3) {
                            if (i < 64) coef_dct[i] = dct4x4[1][64 * block_cnt8x8 + i];
                        } else if (acs.RawStrategy() == 4) {
                            if (i < 256) coef_dct[i] = dct16x16[1][16 * 16 * block_cnt16x16 + i];
                        } else if (acs.RawStrategy() == 5) {
                            coef_dct[i] = dct32x32[1][32 * 32 * block_cnt32x32 + i];
                        } else {
                            std::cout << "unsupported DCT" << std::endl;
                        }
                    }

                    float* coef_dc = dc_rows[1] + bx;

#ifdef XLNX_QC_DEBUG_ENC_GROUP_DC
                    if (acs.RawStrategy() == 5) {
                        std::cout << "========================debug===================== "
                                     "convered blocks: "
                                  << acs.covered_blocks_x() << " tile_xsize: " << tile_xsize
                                  << " bx: " << block_group_rect.x0() << " " << bx << " by: " << block_group_rect.y0()
                                  << " " << by << " dc_stride: " << dc_stride << std::endl;
                        for (int i = 0; i < 4; i++) {
                            for (int j = 0; j < 4; j++) {
                                std::cout << std::setw(15) << coef_dc[i * dc_stride + j] << " ";
                            }
                        }
                        std::cout << std::endl;
                        for (int i = 0; i < 16; i++) {
                            std::cout << std::setw(15) << dc32x32[1][16 * block_cnt32x32 + i] << " ";
                        }
                        std::cout << std::endl;
                        for (int i = 0; i < 4; i++) {
                            for (int j = 0; j < 4; j++) {
                                if (coef_dc[i * dc_stride + j] != dc32x32[1][16 * block_cnt32x32 + i * 4 + j])
                                    std::cout << "!!!";
                            }
                        }
                        std::cout << std::endl;
                    }
#endif

                    if (acs.RawStrategy() == 0) {
                        coef_dc[0] = dc8x8[1][block_cnt8x8];
                    } else if (acs.RawStrategy() == 1) {
                        coef_dc[0] = dcIDT[1][block_cnt8x8];
                    } else if (acs.RawStrategy() == 2) {
                        coef_dc[0] = dc2x2[1][block_cnt8x8];
                    } else if (acs.RawStrategy() == 3) {
                        coef_dc[0] = dc4x4[1][block_cnt8x8];
                    } else if (acs.RawStrategy() == 4) {
                        for (int i = 0; i < 2; i++) {
                            for (int j = 0; j < 2; j++) {
                                coef_dc[i * dc_stride + j] = dc16x16[1][4 * block_cnt16x16 + i * 2 + j];
                            }
                        }
                    } else if (acs.RawStrategy() == 5) {
                        for (int i = 0; i < 4; i++) {
                            for (int j = 0; j < 4; j++) {
                                coef_dc[i * dc_stride + j] = dc32x32[1][16 * block_cnt32x32 + i * 4 + j];
                            }
                        }
                    } else {
                        std::cout << "unsupported DCFromLowFREQ" << std::endl;
                    }

                    QuantizeRoundtripYBlockAC(enc_state->shared.quantizer, error_diffusion, quant_ac, acs.RawStrategy(),
                                              xblocks, yblocks, kDefaultQuantBias, coeffs_in + size, quantized + size);

                    // DCT X and B channels
                    for (size_t c : {0, 2}) {
                        //            TransformFromPixels(acs.Strategy(), opsin_rows[c] + bx
                        //            * kBlockDim,
                        //                                opsin_stride, coeffs_in + c *
                        //                                size, scratch_space);
                        coef_dct = coeffs_in + c * size;
                        for (int i = 0; i < 32 * 32; i++) {
                            if (acs.RawStrategy() == 0) {
                                if (i < 64) coef_dct[i] = dct8x8[c][64 * block_cnt8x8 + i];
                            } else if (acs.RawStrategy() == 1) {
                                if (i < 64) coef_dct[i] = dctIDT[c][64 * block_cnt8x8 + i];
                            } else if (acs.RawStrategy() == 2) {
                                if (i < 64) coef_dct[i] = dct2x2[c][64 * block_cnt8x8 + i];
                            } else if (acs.RawStrategy() == 3) {
                                if (i < 64) coef_dct[i] = dct4x4[c][64 * block_cnt8x8 + i];
                            } else if (acs.RawStrategy() == 4) {
                                if (i < 256) coef_dct[i] = dct16x16[c][16 * 16 * block_cnt16x16 + i];
                            } else if (acs.RawStrategy() == 5) {
                                coef_dct[i] = dct32x32[c][32 * 32 * block_cnt32x32 + i];
                            } else {
                                std::cout << "unsupported DCT" << std::endl;
                            }
                        }
                    }

                    // Unapply color correlation
                    for (size_t k = 0; k < size; k += Lanes(d)) {
                        const auto in_x = Load(d, coeffs_in + k);
                        const auto in_y = Load(d, coeffs_in + size + k);
                        const auto in_b = Load(d, coeffs_in + 2 * size + k);
                        const auto out_x = in_x - x_factor * in_y;
                        const auto out_b = in_b - b_factor * in_y;
                        Store(out_x, d, coeffs_in + k);
                        Store(out_b, d, coeffs_in + 2 * size + k);
                    }

                    // Quantize X and B channels and set DC.
                    for (size_t c : {0, 2}) {
                        QuantizeBlockAC(enc_state->shared.quantizer, error_diffusion, c, quant_ac,
                                        c == 0 ? enc_state->x_qm_multiplier : enc_state->b_qm_multiplier,
                                        acs.RawStrategy(), xblocks, yblocks, coeffs_in + c * size,
                                        quantized + c * size);
                        /*           DCFromLowestFrequencies(acs.Strategy(), coeffs_in + c *
                           size, dc_rows[c] + bx, dc_stride);*/
                        coef_dc = dc_rows[c] + bx;
                        if (acs.RawStrategy() == 0) {
                            coef_dc[0] = dc8x8[c][block_cnt8x8];
                        } else if (acs.RawStrategy() == 1) {
                            coef_dc[0] = dcIDT[c][block_cnt8x8];
                        } else if (acs.RawStrategy() == 2) {
                            coef_dc[0] = dc2x2[c][block_cnt8x8];
                        } else if (acs.RawStrategy() == 3) {
                            coef_dc[0] = dc4x4[c][block_cnt8x8];
                        } else if (acs.RawStrategy() == 4) {
                            for (int i = 0; i < 2; i++) {
                                for (int j = 0; j < 2; j++) {
                                    coef_dc[i * dc_stride + j] = dc16x16[c][4 * block_cnt16x16 + i * 2 + j];
                                }
                            }
                        } else if (acs.RawStrategy() == 5) {
                            for (int i = 0; i < 4; i++) {
                                for (int j = 0; j < 4; j++) {
                                    coef_dc[i * dc_stride + j] = dc32x32[c][16 * block_cnt32x32 + i * 4 + j];
                                }
                            }
                        } else {
                            std::cout << "unsupported DCFromLowFREQ" << std::endl;
                        }
                    }
                    enc_state->progressive_splitter.SplitACCoefficients(quantized, size, acs, bx, by, offset, coeffs);
                    offset += size;
                }
            }
        }
    }
}

// NOLINTNEXTLINE(google-readability-namespace-comments)
} // namespace HWY_NAMESPACE
} // namespace jxl
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace jxl {
HWY_EXPORT(ComputeCoefficients);
void ComputeCoefficients(size_t group_idx,
                         PassesEncoderState* enc_state,
                         const Image3F& opsin,
                         Image3F* dc,
                         //==========acc interface========
                         size_t xsize,
                         size_t ysize,
                         std::vector<std::vector<float> >& dctIDT,
                         std::vector<std::vector<float> >& dct2x2,
                         std::vector<std::vector<float> >& dct4x4,
                         std::vector<std::vector<float> >& dct8x8,
                         std::vector<std::vector<float> >& dct16x16,
                         std::vector<std::vector<float> >& dct32x32,

                         std::vector<std::vector<float> >& dcIDT,
                         std::vector<std::vector<float> >& dc2x2,
                         std::vector<std::vector<float> >& dc4x4,
                         std::vector<std::vector<float> >& dc8x8,
                         std::vector<std::vector<float> >& dc16x16,
                         std::vector<std::vector<float> >& dc32x32
                         //================================
                         ) {
    return HWY_DYNAMIC_DISPATCH(ComputeCoefficients)(group_idx, enc_state, opsin, dc, xsize, ysize, dctIDT, dct2x2,
                                                     dct4x4, dct8x8, dct16x16, dct32x32, dcIDT, dc2x2, dc4x4, dc8x8,
                                                     dc16x16, dc32x32);
}

Status EncodeGroupTokenizedCoefficients(size_t group_idx,
                                        size_t pass_idx,
                                        size_t histogram_idx,
                                        const PassesEncoderState& enc_state,
                                        BitWriter* writer,
                                        AuxOut* aux_out) {
    // Select which histogram to use among those of the current pass.
    const size_t num_histograms = enc_state.shared.num_histograms;
    // num_histograms is 0 only for lossless.
    JXL_ASSERT(num_histograms == 0 || histogram_idx < num_histograms);
    size_t histo_selector_bits = CeilLog2Nonzero(num_histograms);

    if (histo_selector_bits != 0) {
        BitWriter::Allotment allotment(writer, histo_selector_bits);
        writer->Write(histo_selector_bits, histogram_idx);
        ReclaimAndCharge(writer, &allotment, kLayerAC, aux_out);
    }
    WriteTokens(enc_state.passes[pass_idx].ac_tokens[group_idx], enc_state.passes[pass_idx].codes,
                enc_state.passes[pass_idx].context_map, writer, kLayerACTokens, aux_out);

    return true;
}

} // namespace jxl
#endif // HWY_ONCE
