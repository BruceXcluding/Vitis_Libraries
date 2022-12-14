/*
 * Copyright 2020 Xilinx, Inc.
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
#include "utils.hpp"
#include "xf_graph_L3.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>
#include "stdlib.h"
#include <cmath>
#include "xf_utils_sw/logger.hpp"

#define DT uint32_t

double showTimeData2(std::string p_Task, TimePointType& t1, TimePointType& t2) {
    t2 = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> l_durationSec = t2 - t1;
    double l_timeMs = l_durationSec.count() * 1e3;
    std::cout << p_Task << "  " << std::fixed << std::setprecision(6) << l_timeMs << " msec" << std::endl;
    return l_timeMs;
};

class ArgParser {
   public:
    ArgParser(int& argc, const char** argv) {
        for (int i = 1; i < argc; ++i) mTokens.push_back(std::string(argv[i]));
    }
    bool getCmdOption(const std::string option, std::string& value) const {
        std::vector<std::string>::const_iterator itr;
        itr = std::find(this->mTokens.begin(), this->mTokens.end(), option);
        if (itr != this->mTokens.end() && ++itr != this->mTokens.end()) {
            value = *itr;
            return true;
        }
        return false;
    }

   private:
    std::vector<std::string> mTokens;
};

int main(int argc, const char* argv[]) {
    //--------------- cmd parser -------------------------------
    ArgParser parser(argc, argv);
    std::string filenameOffset;
    std::string filenameIndice;
    std::string filenameGolden;
    std::string num_str;
    std::string xclbinPath;
    if (!parser.getCmdOption("-xclbin", num_str)) { // xclbin
        std::cout << "INFO: xclbin file path is not set!\n";
        exit(1);
    } else {
        xclbinPath = num_str;
        std::cout << "INFO: xclbin file path is " << xclbinPath << std::endl;
    }
    if (!parser.getCmdOption("-offset", num_str)) {
        filenameOffset = "./data/csr_offsets.txt";
        std::cout << "INFO: offset file is not set!\n";
    } else {
        filenameOffset = num_str;
    }
    if (!parser.getCmdOption("-indice", num_str)) {
        filenameIndice = "./data/csr_columns.txt";
        std::cout << "INFO: indice file is not set!\n";
    } else {
        filenameIndice = num_str;
    }
    if (!parser.getCmdOption("-golden", num_str)) {
        filenameGolden = "./data/label.txt";
        std::cout << "INFO: golden file is not set!\n";
    } else {
        filenameGolden = num_str;
    }
    std::cout << "INFO: offset file path is " << filenameOffset << std::endl;
    std::cout << "INFO: indice file path is " << filenameIndice << std::endl;

    //----------------- Text Parser --------------------------
    std::string opName;
    std::string kernelName;
    int requestLoad;
    int deviceNeeded;

    std::fstream userInput("./config.json", std::ios::in);
    if (!userInput) {
        std::cout << "Error : file doesn't exist !" << std::endl;
        exit(1);
    }
    char line[1024] = {0};
    char* token;
    while (userInput.getline(line, sizeof(line))) {
        token = strtok(line, "\"\t ,}:{\n");
        while (token != NULL) {
            if (!std::strcmp(token, "operationName")) {
                token = strtok(NULL, "\"\t ,}:{\n");
                opName = token;
            } else if (!std::strcmp(token, "kernelName")) {
                token = strtok(NULL, "\"\t ,}:{\n");
                kernelName = token;
            } else if (!std::strcmp(token, "requestLoad")) {
                token = strtok(NULL, "\"\t ,}:{\n");
                requestLoad = std::atoi(token);
            } else if (!std::strcmp(token, "deviceNeeded")) {
                token = strtok(NULL, "\"\t ,}:{\n");
                deviceNeeded = std::atoi(token);
            }
            token = strtok(NULL, "\"\t ,}:{\n");
        }
    }
    userInput.close();

    //----------------- Setup shortestPathFloat thread ---------
    xf::graph::L3::Handle::singleOP op0;
    op0.operationName = (char*)opName.c_str();
    op0.setKernelName((char*)kernelName.c_str());
    op0.requestLoad = requestLoad;
    op0.xclbinPath = xclbinPath.c_str();
    op0.deviceNeeded = deviceNeeded;

    xf::graph::L3::Handle handle0;
    handle0.addOp(op0);
    handle0.setUp();

    //----------------- Readin Graph from file ---------------------
    bool weighted = 0;
    uint32_t numVertices;
    uint32_t numEdges;
    uint32_t* offsetsCSR;
    uint32_t* indicesCSR;
    DT* weightsCSR;

    readInOffset<uint32_t>(filenameOffset, numVertices, &offsetsCSR);
    readInIndice<uint32_t, DT>(filenameIndice, weighted, numEdges, &indicesCSR, &weightsCSR);

    uint32_t maxVertices = 16 * 800000;
    uint32_t maxEdges = 16 * 800000;
    xf::graph::Graph<uint32_t, DT> g("CSR", maxVertices, maxEdges);
    g.allocBuffer("CSC", maxVertices, maxEdges);
    g.nodeNum = numVertices;
    g.edgeNum = numEdges;
    for (int i = 0; i < numVertices + 1; ++i) {
        g.offsetsCSR[i] = offsetsCSR[i];
    }
    for (int i = 0; i < numEdges; ++i) {
        g.indicesCSR[i] = indicesCSR[i];
    }

    delete[] offsetsCSR;
    delete[] indicesCSR;
    delete[] weightsCSR;
    uint32_t num_runs = 1;
    std::cout << "INFO: Number of kernel runs: " << num_runs << std::endl;
    std::cout << "INFO: Number of nodes: " << numVertices << std::endl;
    std::cout << "INFO: Number of edges: " << numEdges << std::endl;
    uint32_t maxIter = 1;
    uint32_t* labels = xf::graph::L3::aligned_alloc<uint32_t>(maxVertices);
    uint32_t* labelGolden = new uint32_t[numVertices];
    uint32_t tmp;
    std::fstream fileLabel(filenameGolden.c_str(), std::ios::in);
    if (!fileLabel) {
        std::cout << "Error : " << filenameGolden << " file doesn't exist !" << std::endl;
        exit(1);
    }

    uint32_t index = 0;
    while (fileLabel.getline(line, sizeof(line))) {
        std::stringstream data(line);
        data >> tmp;
        data >> tmp;
        if (index > 0) {
            labelGolden[index - 1] = tmp;
        }
        index++;
    }

    //---------------- Run L3 API -----------------------------------
    auto ev = xf::graph::L3::labelPropagation(handle0, maxIter, g, labels);
    int ret = ev.wait();

    //---------------- Check Result ---------------------------------
    uint32_t err = 0;
    for (int i = 0; i < numVertices; ++i) {
        err += std::abs((int)(labels[i] - labelGolden[i]));
    }

    //--------------- Free and delete -----------------------------------
    handle0.free();
    g.freeBuffers();
    delete[] labels;
    delete[] labelGolden;
    xf::common::utils_sw::Logger logger(std::cout, std::cerr);
    if (err == 0) {
        logger.info(xf::common::utils_sw::Logger::Message::TEST_PASS);
        return 0;
    } else {
        logger.error(xf::common::utils_sw::Logger::Message::TEST_FAIL);
        return 1;
    }
}
