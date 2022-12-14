/*
 * Copyright 2019 Xilinx, Inc.
 *
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

#include "aes256CbcDecrypt_acc.hpp"
#include <ap_int.h>
#include <hls_stream.h>
#include "xf_security/cbc.hpp"
#include "xf_security/msgpack.hpp"
#include "aes256CbcDecrypt_acc.hpp"

#ifndef __SYNTHESIS
#include <iostream>
#endif

void aes256CbcDecrypt::compute(int hb_in_size, int hb_out_size, ap_uint<128>* hb_in, ap_uint<128>* hb_out) {
    hls_kernel(hb_in_size, hb_out_size, hb_in, hb_out);
}

void cbcWrapper(hls::stream<ap_uint<128> >& plaintext,
                hls::stream<bool>& plaintext_e,
                hls::stream<ap_uint<256> >& key,
                hls::stream<ap_uint<128> >& iv,
                hls::stream<ap_uint<128> >& res,
                hls::stream<bool>& endRes,
                ap_uint<64> msgNum) {
    for (ap_uint<64> i = 0; i < msgNum; i++) {
        xf::security::aes256CbcDecrypt(plaintext, plaintext_e, key, iv, res, endRes);
    }
}

void wrapper(ap_uint<128>* input, ap_uint<128>* output, ap_uint<64> msg_num, ap_uint<64> row_num) {
#pragma HLS dataflow

    hls::stream<ap_uint<128> > textStrm;
#pragma HLS stream variable = textStrm depth = 128
#pragma HLS resource variable = textStrm core = FIFO_LUTRAM
    hls::stream<bool> endTextStrm;
#pragma HLS stream variable = endTextStrm depth = 128
#pragma HLS resource variable = endTextStrm core = FIFO_LUTRAM

    hls::stream<ap_uint<256> > keyStrm;
#pragma HLS stream variable = keyStrm depth = 4
#pragma HLS resource variable = keyStrm core = FIFO_LUTRAM

    hls::stream<ap_uint<128> > ivStrm;
#pragma HLS stream variable = ivStrm depth = 4
#pragma HLS resource variable = ivStrm core = FIFO_LUTRAM

    hls::stream<ap_uint<64> > lenStrm;
#pragma HLS stream variable = lenStrm depth = 4
#pragma HLS resource variable = lenStrm core = FIFO_LUTRAM

    xf::security::internal::aesCbcPack<256> packer;
    packer.scanPack(input, msg_num, row_num, textStrm, endTextStrm, keyStrm, ivStrm, lenStrm);

    hls::stream<ap_uint<128> > resStrm;
#pragma HLS stream variable = resStrm depth = 128
#pragma HLS resource variable = resStrm core = FIFO_LUTRAM

    hls::stream<bool> endResStrm;
#pragma HLS stream variable = endResStrm depth = 128
#pragma HLS resource variable = endResStrm core = FIFO_LUTRAM

    cbcWrapper(textStrm, endTextStrm, keyStrm, ivStrm, resStrm, endResStrm, msg_num);

    packer.writeOutMsgPack(output, msg_num, resStrm, endResStrm, lenStrm);
}

// @brief top of kernel
void aes256CbcDecrypt::hls_kernel(int hb_in_size, int hb_out_size, ap_uint<128>* hb_in, ap_uint<128>* hb_out) {
    ap_uint<128> tmp = hb_in[0];
    ap_uint<64> msg_num = tmp.range(63, 0);
    ap_uint<64> row_num = tmp.range(127, 64);
    wrapper(hb_in, hb_out, msg_num, row_num);
} // end aes256CbcDecryptKernel
