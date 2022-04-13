/*
 * Copyright 2022 Xilinx, Inc.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "fir_decimate_asym_ref.hpp"
#include "fir_ref_utils.hpp"

/*
Decimator Asymmetric FIR filter reference model
*/

namespace xf {
namespace dsp {
namespace aie {
namespace fir {
namespace decimate_asym {

//-----------------------------------------------------------------------------------------------------
// REF FIR function - static coefficients, single output
template <typename TT_DATA,
          typename TT_COEFF,
          unsigned int TP_FIR_LEN,
          unsigned int TP_DECIMATE_FACTOR,
          unsigned int TP_SHIFT,
          unsigned int TP_RND,
          unsigned int TP_INPUT_WINDOW_VSIZE,
          unsigned int TP_USE_COEFF_RELOAD,
          unsigned int TP_NUM_OUTPUTS,
          unsigned int TP_DUAL_IP,
          unsigned int TP_API>
void filter_ref(input_window<TT_DATA>* inWindow,
                output_window<TT_DATA>* outWindow,
                const TT_COEFF (&taps)[TP_FIR_LEN]) {
    const unsigned int shift = TP_SHIFT;
    T_accRef<TT_DATA> accum;
    TT_DATA d_in[TP_FIR_LEN];
    TT_DATA accumSrs;

    const unsigned int kFirMarginOffset = fnFirMargin<TP_FIR_LEN, TT_DATA>() - TP_FIR_LEN + 1; // FIR Margin Offset.
    window_incr(inWindow, kFirMarginOffset);                                                   // read input data

    for (unsigned int i = 0; i < TP_INPUT_WINDOW_VSIZE / TP_DECIMATE_FACTOR; i++) {
        accum = null_accRef<TT_DATA>(); // reset accumulator at the start of the mult-add for each output sample
        // Accumulation
        for (unsigned int j = 0; j < TP_FIR_LEN; j++) {
            d_in[j] = window_readincr(inWindow); // read input data

            // Note the coefficient index reversal. See note in constructor.
            multiplyAcc<TT_DATA, TT_COEFF>(accum, d_in[j], taps[TP_FIR_LEN - 1 - j]);
        }
        // Revert data pointer for next sample
        window_decr(inWindow, TP_FIR_LEN - TP_DECIMATE_FACTOR);

        roundAcc(TP_RND, shift, accum);
        saturateAcc(accum);
        accumSrs = castAcc(accum);
        window_writeincr((output_window<TT_DATA>*)outWindow, accumSrs);
    }
};
//-----------------------------------------------------------------------------------------------------
// REF FIR function - static coefficients, single output
template <typename TT_DATA,
          typename TT_COEFF,
          unsigned int TP_FIR_LEN,
          unsigned int TP_DECIMATE_FACTOR,
          unsigned int TP_SHIFT,
          unsigned int TP_RND,
          unsigned int TP_INPUT_WINDOW_VSIZE,
          unsigned int TP_USE_COEFF_RELOAD,
          unsigned int TP_NUM_OUTPUTS,
          unsigned int TP_DUAL_IP,
          unsigned int TP_API>
void fir_decimate_asym_ref<TT_DATA,
                           TT_COEFF,
                           TP_FIR_LEN,
                           TP_DECIMATE_FACTOR,
                           TP_SHIFT,
                           TP_RND,
                           TP_INPUT_WINDOW_VSIZE,
                           TP_USE_COEFF_RELOAD,
                           TP_NUM_OUTPUTS,
                           TP_DUAL_IP,
                           TP_API>::filter(input_window<TT_DATA>* inWindow, output_window<TT_DATA>* outWindow) {
    filter_ref<TT_DATA, TT_COEFF, TP_FIR_LEN, TP_DECIMATE_FACTOR, TP_SHIFT, TP_RND, TP_INPUT_WINDOW_VSIZE,
               TP_USE_COEFF_RELOAD, TP_NUM_OUTPUTS, TP_DUAL_IP, TP_API>(inWindow, outWindow, m_internalTaps);
};

// REF FIR function - reloadable coefficients, single output
//-----------------------------------------------------------------------------------------------------
template <typename TT_DATA,
          typename TT_COEFF,
          unsigned int TP_FIR_LEN,
          unsigned int TP_DECIMATE_FACTOR,
          unsigned int TP_SHIFT,
          unsigned int TP_RND,
          unsigned int TP_INPUT_WINDOW_VSIZE,
          unsigned int TP_NUM_OUTPUTS,
          unsigned int TP_DUAL_IP,
          unsigned int TP_API>
void fir_decimate_asym_ref<TT_DATA,
                           TT_COEFF,
                           TP_FIR_LEN,
                           TP_DECIMATE_FACTOR,
                           TP_SHIFT,
                           TP_RND,
                           TP_INPUT_WINDOW_VSIZE,
                           USE_COEFF_RELOAD_TRUE,
                           TP_NUM_OUTPUTS,
                           TP_DUAL_IP,
                           TP_API>::filter(input_window<TT_DATA>* inWindow,
                                           output_window<TT_DATA>* outWindow,
                                           const TT_COEFF (&inTaps)[TP_FIR_LEN]) {
    // Coefficient reload
    for (int i = 0; i < TP_FIR_LEN; i++) {
        m_internalTaps[i] = inTaps[i];
    }
    filter_ref<TT_DATA, TT_COEFF, TP_FIR_LEN, TP_DECIMATE_FACTOR, TP_SHIFT, TP_RND, TP_INPUT_WINDOW_VSIZE,
               USE_COEFF_RELOAD_TRUE, TP_NUM_OUTPUTS, TP_DUAL_IP, TP_API>(inWindow, outWindow, m_internalTaps);
};
}
}
}
}
}
