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
#ifndef _DSPLIB_FIR_DECIMATE_ASYM_REF_HPP_
#define _DSPLIB_FIR_DECIMATE_ASYM_REF_HPP_

/*
Asymmetric Decimation FIR filter reference model
*/

#include <adf.h>
#include <limits>
#include "fir_ref_utils.hpp"

namespace xf {
namespace dsp {
namespace aie {
namespace fir {
namespace decimate_asym {

//-----------------------------------------------------------------------------------------------------
// Decimate Asym Reference Model Class - static coefficients, single output
template <typename TT_DATA,  // type of data input and output
          typename TT_COEFF, // type of coefficients           (e.g. int16, cint32)
          unsigned int TP_FIR_LEN,
          unsigned int TP_DECIMATE_FACTOR,
          unsigned int TP_SHIFT,
          unsigned int TP_RND,
          unsigned int TP_INPUT_WINDOW_VSIZE,
          unsigned int TP_USE_COEFF_RELOAD = 0, // 1 = use coeff reload, 0 = don't use coeff reload
          unsigned int TP_NUM_OUTPUTS = 1,
          unsigned int TP_DUAL_IP = 0,
          unsigned int TP_API = 0>
class fir_decimate_asym_ref {
   public:
    // Constructor
    fir_decimate_asym_ref(const TT_COEFF (&taps)[TP_FIR_LEN]) {
        for (int i = 0; i < TP_FIR_LEN; i++) {
            m_internalTaps[i] = taps[i];
        }
    }

    // Register Kernel Class
    static void registerKernelClass() { REGISTER_FUNCTION(fir_decimate_asym_ref::filter); }

    // FIR
    void filter(input_window<TT_DATA>* inWindow, output_window<TT_DATA>* outWindow);

   private:
    TT_COEFF chess_storage(% chess_alignof(v8cint16)) m_internalTaps[TP_FIR_LEN];
};

//-----------------------------------------------------------------------------------------------------
// Decimate Asym Reference Model Class - reloadable coefficients, single output
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
class fir_decimate_asym_ref<TT_DATA,
                            TT_COEFF,
                            TP_FIR_LEN,
                            TP_DECIMATE_FACTOR,
                            TP_SHIFT,
                            TP_RND,
                            TP_INPUT_WINDOW_VSIZE,
                            USE_COEFF_RELOAD_TRUE,
                            TP_NUM_OUTPUTS,
                            TP_DUAL_IP,
                            TP_API> {
   public:
    // Constructor
    fir_decimate_asym_ref() {}
    // Register Kernel Class
    static void registerKernelClass() { REGISTER_FUNCTION(fir_decimate_asym_ref::filter); }
    // FIR
    void filter(input_window<TT_DATA>* inWindow,
                output_window<TT_DATA>* outWindow,
                const TT_COEFF (&inTaps)[TP_FIR_LEN]);

   private:
    TT_COEFF chess_storage(% chess_alignof(v8cint16)) m_internalTaps[TP_FIR_LEN];
};
}
}
}
}
}

#endif // _DSPLIB_FIR_DECIMATE_ASYM_REF_HPP_
