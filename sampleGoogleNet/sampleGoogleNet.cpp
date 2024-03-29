/*
 * Copyright 1993-2019 NVIDIA Corporation.  All rights reserved.
 *
 * NOTICE TO LICENSEE:
 *
 * This source code and/or documentation ("Licensed Deliverables") are
 * subject to NVIDIA intellectual property rights under U.S. and
 * international Copyright laws.
 *
 * These Licensed Deliverables contained herein is PROPRIETARY and
 * CONFIDENTIAL to NVIDIA and is being provided under the terms and
 * conditions of a form of NVIDIA software license agreement by and
 * between NVIDIA and Licensee ("License Agreement") or electronically
 * accepted by Licensee.  Notwithstanding any terms or conditions to
 * the contrary in the License Agreement, reproduction or disclosure
 * of the Licensed Deliverables to any third party without the express
 * written consent of NVIDIA is prohibited.
 *
 * NOTWITHSTANDING ANY TERMS OR CONDITIONS TO THE CONTRARY IN THE
 * LICENSE AGREEMENT, NVIDIA MAKES NO REPRESENTATION ABOUT THE
 * SUITABILITY OF THESE LICENSED DELIVERABLES FOR ANY PURPOSE.  IT IS
 * PROVIDED "AS IS" WITHOUT EXPRESS OR IMPLIED WARRANTY OF ANY KIND.
 * NVIDIA DISCLAIMS ALL WARRANTIES WITH REGARD TO THESE LICENSED
 * DELIVERABLES, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY,
 * NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE.
 * NOTWITHSTANDING ANY TERMS OR CONDITIONS TO THE CONTRARY IN THE
 * LICENSE AGREEMENT, IN NO EVENT SHALL NVIDIA BE LIABLE FOR ANY
 * SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, OR ANY
 * DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THESE LICENSED DELIVERABLES.
 *
 * U.S. Government End Users.  These Licensed Deliverables are a
 * "commercial item" as that term is defined at 48 C.F.R. 2.101 (OCT
 * 1995), consisting of "commercial computer software" and "commercial
 * computer software documentation" as such terms are used in 48
 * C.F.R. 12.212 (SEPT 1995) and is provided to the U.S. Government
 * only as a commercial end item.  Consistent with 48 C.F.R.12.212 and
 * 48 C.F.R. 227.7202-1 through 227.7202-4 (JUNE 1995), all
 * U.S. Government End Users acquire the Licensed Deliverables with
 * only those rights set forth herein.
 *
 * Any use of the Licensed Deliverables in individual and commercial
 * software must include, in the user documentation and internal
 * comments to the code, the above Disclaimer and U.S. Government End
 * Users Notice.
 */

//!
//! sampleGoogleNet.cpp
//! This file contains the implementation of the GoogleNet sample. It creates the network using
//! the GoogleNet caffe model.
//! It can be run with the following command line:
//! Command: ./sample_googlenet [-h or --help] [-d=/path/to/data/dir or --datadir=/path/to/data/dir]
//!

#include "argsParser.h"
#include "buffers.h"
#include "logger.h"
#include "common.h"

#include "NvCaffeParser.h"
#include "NvInfer.h"
#include <cuda_runtime_api.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

const std::string gSampleName = "TensorRT.sample_googlenet";

//!
//! \brief  The SampleGoogleNet class implements the GoogleNet sample
//!
//! \details It creates the network using a caffe model
//!
class SampleGoogleNet
{
    template <typename T>
    using SampleUniquePtr = std::unique_ptr<T, samplesCommon::InferDeleter>;

public:
    SampleGoogleNet(const samplesCommon::CaffeSampleParams& params)
        : mParams(params)
    {
    }

    //!
    //! \brief Function builds the network engine
    //!
    bool build();

    //!
    //! \brief This function runs the TensorRT inference engine for this sample
    //!
    bool infer();

    //!
    //! \brief This function can be used to clean up any state created in the sample class
    //!
    bool teardown();

    samplesCommon::CaffeSampleParams mParams;

private:
    std::shared_ptr<nvinfer1::ICudaEngine> mEngine = nullptr; //!< The TensorRT engine used to run the network

    //!
    //! \brief This function parses a Caffe model for GoogleNet and creates a TensorRT network
    //!
    void constructNetwork(SampleUniquePtr<nvinfer1::IBuilder>& builder, SampleUniquePtr<nvinfer1::INetworkDefinition>& network, SampleUniquePtr<nvcaffeparser1::ICaffeParser>& parser);
};

//!
//! \brief This function creates the network, configures the builder and creates the network engine
//!
//! \details This function creates the GoogleNet network by parsing the caffe model and builds
//!          the engine that will be used to run GoogleNet (mEngine)
//!
//! \return Returns true if the engine was created successfully and false otherwise
//!
bool SampleGoogleNet::build()
{
    auto builder = SampleUniquePtr<nvinfer1::IBuilder>(nvinfer1::createInferBuilder(gLogger.getTRTLogger()));
    if (!builder)
        return false;

    auto network = SampleUniquePtr<nvinfer1::INetworkDefinition>(builder->createNetwork());
    if (!network)
        return false;

    auto parser = SampleUniquePtr<nvcaffeparser1::ICaffeParser>(nvcaffeparser1::createCaffeParser());
    if (!parser)
        return false;

    constructNetwork(builder, network, parser);

    mEngine = std::shared_ptr<nvinfer1::ICudaEngine>(builder->buildCudaEngine(*network), samplesCommon::InferDeleter());
    if (!mEngine)
        return false;

    return true;
}

//!
//! \brief This function uses a caffe parser to create the googlenet Network and marks the
//!        output layers
//!
//! \param network Pointer to the network that will be populated with the googlenet network
//!
//! \param builder Pointer to the engine builder
//!
void SampleGoogleNet::constructNetwork(SampleUniquePtr<nvinfer1::IBuilder>& builder, SampleUniquePtr<nvinfer1::INetworkDefinition>& network, SampleUniquePtr<nvcaffeparser1::ICaffeParser>& parser)
{
    const nvcaffeparser1::IBlobNameToTensor* blobNameToTensor = parser->parse(
        locateFile(mParams.prototxtFileName, mParams.dataDirs).c_str(),
        locateFile(mParams.weightsFileName, mParams.dataDirs).c_str(),
        *network,
        nvinfer1::DataType::kFLOAT);

    for (auto& s : mParams.outputTensorNames)
        network->markOutput(*blobNameToTensor->find(s.c_str()));

    builder->setMaxBatchSize(mParams.batchSize);
    builder->setMaxWorkspaceSize(16_MB);
    samplesCommon::enableDLA(builder.get(), mParams.dlaCore);
}

//!
//! \brief This function runs the TensorRT inference engine for this sample
//!
//! \details This function is the main execution function of the sample. It allocates the buffer,
//!          sets inputs and executes the engine.
//!
bool SampleGoogleNet::infer()
{
    // Create RAII buffer manager object
    samplesCommon::BufferManager buffers(mEngine, mParams.batchSize);

    auto context = SampleUniquePtr<nvinfer1::IExecutionContext>(mEngine->createExecutionContext());
    if (!context)
        return false;

    // Fetch host buffers and set host input buffers to all zeros
    for (auto& input : mParams.inputTensorNames)
    {
        const auto bufferSize = buffers.size(input);
        if (bufferSize == samplesCommon::BufferManager::kINVALID_SIZE_VALUE)
        {
            gLogError << "input tensor missing: " << input << "\n";
            exit(EXIT_FAILURE);
        }
        memset(buffers.getHostBuffer(input), 0, bufferSize);
    }

    // Memcpy from host input buffers to device input buffers
    buffers.copyInputToDevice();

    bool status = context->execute(mParams.batchSize, buffers.getDeviceBindings().data());
    if (!status)
        return false;

    // Memcpy from device output buffers to host output buffers
    buffers.copyOutputToHost();

    return true;
}

//!
//! \brief This function can be used to clean up any state created in the sample class
//!
bool SampleGoogleNet::teardown()
{
    //! Clean up the libprotobuf files as the parsing is complete
    //! \note It is not safe to use any other part of the protocol buffers library after
    //! ShutdownProtobufLibrary() has been called.
    nvcaffeparser1::shutdownProtobufLibrary();
    return true;
}

//!
//! \brief This function initializes members of the params struct using the command line args
//!
samplesCommon::CaffeSampleParams initializeSampleParams(const samplesCommon::Args& args)
{
    samplesCommon::CaffeSampleParams params;
    if (args.dataDirs.size() != 0) //!< Use the data directory provided by the user
        params.dataDirs = args.dataDirs;
    else //!< Use default directories if user hasn't provided directory paths
    {
        params.dataDirs.push_back("data/googlenet/");
        params.dataDirs.push_back("data/samples/googlenet/");
    }
    params.prototxtFileName = "googlenet.prototxt";
    params.weightsFileName = "googlenet.caffemodel";
    params.inputTensorNames.push_back("data");
    params.batchSize = 4;
    params.outputTensorNames.push_back("prob");
    params.dlaCore = args.useDLACore;

    return params;
}
//!
//! \brief This function prints the help information for running this sample
//!
void printHelpInfo()
{
    std::cout << "Usage: ./sample_googlenet [-h or --help] [-d or --datadir=<path to data directory>] [--useDLACore=<int>]\n";
    std::cout << "--help          Display help information\n";
    std::cout << "--datadir       Specify path to a data directory, overriding the default. This option can be used multiple times to add multiple directories. If no data directories are given, the default is to use data/samples/googlenet/ and data/googlenet/" << std::endl;
    std::cout << "--useDLACore=N  Specify a DLA engine for layers that support DLA. Value can range from 0 to n-1, where n is the number of DLA engines on the platform." << std::endl;
}

int main(int argc, char** argv)
{
    samplesCommon::Args args;
    bool argsOK = samplesCommon::parseArgs(args, argc, argv);
    if (args.help)
    {
        printHelpInfo();
        return EXIT_SUCCESS;
    }
    if (!argsOK)
    {
        gLogError << "Invalid arguments" << std::endl;
        printHelpInfo();
        return EXIT_FAILURE;
    }

    auto sampleTest = gLogger.defineTest(gSampleName, argc, const_cast<const char**>(argv));

    gLogger.reportTestStart(sampleTest);

    samplesCommon::CaffeSampleParams params = initializeSampleParams(args);
    SampleGoogleNet sample(params);

    gLogInfo << "Building and running a GPU inference engine for GoogleNet" << std::endl;

    if (!sample.build())
    {
        return gLogger.reportFail(sampleTest);
    }
    if (!sample.infer())
    {
        return gLogger.reportFail(sampleTest);
    }
    if (!sample.teardown())
    {
        return gLogger.reportFail(sampleTest);
    }

    gLogInfo << "Ran " << argv[0] << " with: " << std::endl;

    std::stringstream ss;

    ss << "Input(s): ";
    for (auto& input : sample.mParams.inputTensorNames)
        ss << input << " ";
    gLogInfo << ss.str() << std::endl;

    ss.str(std::string());

    ss << "Output(s): ";
    for (auto& output : sample.mParams.outputTensorNames)
        ss << output << " ";
    gLogInfo << ss.str() << std::endl;

    return gLogger.reportPass(sampleTest);
}
