/*
 * Copyright 2022 Xilinx, Inc.
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

#ifndef _XF_GRAPH_MIS_KERNEL_HPP_
#define _XF_GRAPH_MIS_KERNEL_HPP_

// #include "xf_graph_L2.hpp"
#include "hw/maximal_independent_set.hpp"

#include <math.h>
#include <time.h>
#include <iostream>
#include "hls_stream.h"

#define MAX_ROUND 100
const int M_VERTEX = 57108864;
const int M_EDGE = 134217728;

extern "C" void mis_kernel(int m,
                           const int* offset,
                           const int* indices,
                           int* C_group_0,
                           int* C_group_1,
                           int* S_group_0,
                           int* S_group_1,
                           int* res_out);

#endif