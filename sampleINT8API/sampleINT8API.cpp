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

//! sampleINT8API.cpp
//! This file contains implementation showcasing usage of INT8 calibration and precision APIs.
//! It creates classification networks such as mobilenet, vgg19, resnet-50 from onnx model file.
//! This sample showcae setting per tensor dynamic range overriding calibrator generated scales if it exists.
//! This sample showcase how to set computation precision of layer. It involves forcing output tensor type of the layer to particular precision.
//! It can be run with the following command line:
//! Command: ./sample_int8_api [-h or --help] [-m modelfile] [-s per_tensor_dynamic_range_file] [-i image_file] [-r reference_file] [-d path/to/data/dir] [--verbose] [-useDLA <id>]

#include "logger.h"
#include "common.h"
#include "buffers.h"
#include "argsParser.h"

#include "NvInfer.h"
#include "NvOnnxParser.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <unordered_map>
#include <cuda_runtime_api.h>

using namespace nvinfer1;

const std::string gSampleName = "TensorRT.sample_int8_api";

static const int kINPUT_C = 3;
static const int kINPUT_H = 224;
static const int kINPUT_W = 224;

// Preprocessing values are available here: https://github.com/onnx/models/tree/master/models/image_classification/resnet
static const float kMean[3] = {0.485f, 0.456f, 0.406f};
static const float kStdDev[3] = {0.229f, 0.224f, 0.225f};
static const float kScale = 255.0f;

//!
//! \brief The SampleINT8APIParams structure groups the additional parameters required by
//!         the INT8 API sample
//!
struct SampleINT8APIParams
{
    bool verbose{false};
    bool writeNetworkTensors{false};
    int dlaCore{-1};
    int batchSize;
    std::string modelFileName;
    vector<std::string> dataDirs;
    std::string dynamicRangeFileName;
    std::string imageFileName;
    std::string referenceFileName;
    std::string networkTensorsFileName;
};

//!
//! \brief The SampleINT8APIArgs structures groups the additional arguments required by
//!         the INT8 API sample
//!
struct SampleINT8APIArgs : public samplesCommon::Args
{
    bool verbose{false};
    bool writeNetworkTensors{false};
    std::string modelFileName{"resnet50.onnx"};
    std::string imageFileName{"airliner.ppm"};
    std::string referenceFileName{"reference_labels.txt"};
    std::string dynamicRangeFileName{"resnet50_per_tensor_dynamic_range.txt"};
    std::string networkTensorsFileName{"network_tensors.txt"};
};

//!
//! \brief This function prints the help information for running this sample
//!
void printHelpInfo()
{
    std::cout << "Usage: ./sample_int8_api [-h or --help] [--model=model_file] [--ranges=per_tensor_dynamic_range_file] [--image=image_file] [--reference=reference_file] [--data=/path/to/data/dir] [--useDLACore=<int>] [-v or --verbose]\n";
    std::cout << "-h or --help. Display This help information" << std::endl;
    std::cout << "--model=model_file.onnx or /absolute/path/to/model_file.onnx. Generate model file using README.md in case it does not exists. Default to resnet50.onnx" << std::endl;
    std::cout << "--image=image.ppm or /absolute/path/to/image.ppm. Image to infer. Defaults to airlines.ppm" << std::endl;
    std::cout << "--reference=reference.txt or /absolute/path/to/reference.txt. Reference labels file. Defaults to reference_labels.txt" << std::endl;
    std::cout << "--ranges=ranges.txt or /absolute/path/to/ranges.txt. Specify custom per tensor dynamic range for the network. Defaults to resnet50_per_tensor_dynamic_range.txt" << std::endl;
    std::cout << "--write_tensors. Option to generate file containing network tensors name. By default writes to network_tensors.txt file. To provide user defined file name use additional option --network_tensors_file. See --network_tensors_file option usage for more detail." << std::endl;
    std::cout << "--network_tensors_file=network_tensors.txt or /absolute/path/to/network_tensors.txt. This option needs to be used with --write_tensors option. Specify file name (will write to current execution directory) or absolute path to file name to write network tensor names file. Dynamic range corresponding to each network tensor is required to run the sample. Defaults to network_tensors.txt" << std::endl;
    std::cout << "--data=/path/to/data/dir. Specify data directory to search for above files in case absolute paths to files are not provided. Defaults to data/samples/int8_api/ or data/int8_api/" << std::endl;
    std::cout << "--useDLACore=N. Specify a DLA engine for layers that support DLA. Value can range from 0 to n-1, where n is the number of DLA engines on the platform." << std::endl;
    std::cout << "--verbose. Outputs per tensor dynamic range and layer precision info for the network" << std::endl;
}

//!
//! \brief This function parses arguments specific to sampleINT8API
//!
bool parseSampleINT8APIArgs(SampleINT8APIArgs& args, int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        if (!strncmp(argv[i], "--model=", 8))
        {
            args.modelFileName = (argv[i] + 8);
        }
        else if (!strncmp(argv[i], "--image=", 8))
        {
            args.imageFileName = (argv[i] + 8);
        }
        else if (!strncmp(argv[i], "--reference=", 12))
        {
            args.referenceFileName = (argv[i] + 12);
        }
        else if (!strncmp(argv[i], "--write_tensors", 15))
        {
            args.writeNetworkTensors = true;
        }
        else if (!strncmp(argv[i], "--network_tensors_file=", 23))
        {
            args.networkTensorsFileName = (argv[i] + 23);
        }
        else if (!strncmp(argv[i], "--ranges=", 9))
        {
            args.dynamicRangeFileName = (argv[i] + 9);
        }
        else if (!strncmp(argv[i], "--int8", 6))
        {
            args.runInInt8 = true;
        }
        else if (!strncmp(argv[i], "--fp16", 6))
        {
            args.runInFp16 = true;
        }
        else if (!strncmp(argv[i], "--useDLACore=", 13))
        {
            args.useDLACore = std::stoi(argv[i] + 13);
        }
        else if (!strncmp(argv[i], "--data=", 7))
        {
            std::string dirPath = (argv[i] + 7);
            if (dirPath.back() != '/')
            {
                dirPath.push_back('/');
            }
            args.dataDirs.push_back(dirPath);
        }
        else if (!strncmp(argv[i], "--verbose", 9) || !strncmp(argv[i], "-v", 2) )
        {
            args.verbose = true;
        }
        else if (!strncmp(argv[i], "--help", 6) || !strncmp(argv[i], "-h", 2))
        {
            args.help = true;
        }
        else
        {
           gLogError << "Invalid Argument: " << argv[i] << std::endl;
           return false;
        }
    }
    return true;
}

void validateInputParams(SampleINT8APIParams& params)
{
    gLogInfo << "Please follow README.md to generate missing input files." << std::endl;
    gLogInfo << "Validating input parameters. Using following input files for inference." << std::endl;
    params.modelFileName = locateFile(params.modelFileName, params.dataDirs);
    gLogInfo << "    Model File: " << params.modelFileName << std::endl;
    if (params.writeNetworkTensors)
    {
        gLogInfo << "    Writing Network Tensors File to: " << params.networkTensorsFileName << std::endl;
        return;
    }
    params.imageFileName = locateFile(params.imageFileName, params.dataDirs);
    gLogInfo << "    Image File: " << params.imageFileName << std::endl;
    params.referenceFileName = locateFile(params.referenceFileName, params.dataDirs);
    gLogInfo << "    Reference File: " << params.referenceFileName << std::endl;
    params.dynamicRangeFileName = locateFile(params.dynamicRangeFileName, params.dataDirs);
    gLogInfo << "    Dynamic Range File: " << params.dynamicRangeFileName << std::endl;
    return;
}

//!
//! \brief This function initializes members of the params struct using the command line args
//!
void initializeSampleParams(SampleINT8APIArgs args, SampleINT8APIParams& params)
{
    if (args.dataDirs.size() != 0) //!< Use the data directory provided by the user
    {
        params.dataDirs = args.dataDirs;
    }
    else //!< Use default directories if user hasn't provided directory paths
    {
        params.dataDirs.push_back("data/samples/int8_api/");
        params.dataDirs.push_back("data/int8_api/");
    }

    params.dataDirs.push_back(""); //! In case of absolute path search
    params.batchSize = 1;
    params.verbose = args.verbose;
    params.modelFileName = args.modelFileName;
    params.imageFileName = args.imageFileName;
    params.referenceFileName = args.referenceFileName;
    params.dynamicRangeFileName = args.dynamicRangeFileName;
    params.dlaCore = args.useDLACore;
    params.writeNetworkTensors = args.writeNetworkTensors;
    params.networkTensorsFileName = args.networkTensorsFileName;
    validateInputParams(params);
    return;
}

//!
//! \brief The sampleINT8API class implements INT8 inference on classification networks.
//!
//! \details INT8 API usage for setting custom int8 range for each input layer. API showcase how
//!           to perform INT8 inference without calibration table
//!
class sampleINT8API
{
private:
    template <typename T>
    using SampleUniquePtr = std::unique_ptr<T, samplesCommon::InferDeleter>;

public:
    sampleINT8API(const SampleINT8APIParams& params)
        : mParams(params)
    {
    }

    //!
    //! \brief Function builds the network engine
    //!
    Logger::TestResult build();

    //!
    //! \brief This function runs the TensorRT inference engine for this sample
    //!
    Logger::TestResult infer();

    //!
    //! \brief This function can be used to clean up any state created in the sample class
    //!
    Logger::TestResult teardown();

    SampleINT8APIParams mParams; //!< Stores Sample Parameter

private:
    std::shared_ptr<nvinfer1::ICudaEngine> mEngine = nullptr; //!< The TensorRT engine used to run the network

    std::map<std::string, std::string> mInOut; //!< Input and output mapping of the network

    nvinfer1::Dims mInputDims; //!< The dimensions of the input to the network

    nvinfer1::Dims mOutputDims; //!< The dimensions of the output to the network

    std::unordered_map<std::string, float> mPerTensorDynamicRangeMap; //!< Mapping from tensor name to max absolute dynamic range values

    void getInputOutputNames(); //!< Populates input and output mapping of the network

    //!
    //! \brief Reads the ppm input image, preprocesses, and stores the result in a managed buffer
    //!
    bool prepareInput(const samplesCommon::BufferManager& buffers);

    //!
    //! \brief Verifies that the output is correct and prints it
    //!
    bool verifyOutput(const samplesCommon::BufferManager& buffers) const;

    //!
    //! \brief Populate per tensor dynamic range values
    //!
    bool readPerTensorDynamicRangeValues();

    //!
    //! \brief  Sets custom dynamic range for network tensors
    //!
    bool setDynamicRange(SampleUniquePtr<nvinfer1::INetworkDefinition>& network);

    //!
    //! \brief  Sets computation precision for network layers
    //!
    void setLayerPrecision(SampleUniquePtr<nvinfer1::INetworkDefinition>& network);

    //!
    //! \brief  Write network tensor names to a file.
    //!
    void writeNetworkTensorNames(const SampleUniquePtr<nvinfer1::INetworkDefinition>& network);
};

//!
//! \brief  Populates input and output mapping of the network
//!
void sampleINT8API::getInputOutputNames()
{
    int nbindings = mEngine.get()->getNbBindings();
    assert(nbindings == 2);
    for (int b = 0; b < nbindings; ++b)
    {
        nvinfer1::Dims dims = mEngine.get()->getBindingDimensions(b);
        if (mEngine.get()->bindingIsInput(b))
        {
            if (mParams.verbose)
                gLogInfo << "Found input: "
                         << mEngine.get()->getBindingName(b)
                         << " shape=" << dims
                         << " dtype=" << (int) mEngine.get()->getBindingDataType(b)
                         << std::endl;
            mInOut["input"] = mEngine.get()->getBindingName(b);
        }
        else
        {
            if (mParams.verbose)
                gLogInfo << "Found output: "
                         << mEngine.get()->getBindingName(b)
                         << " shape=" << dims
                         << " dtype=" << (int) mEngine.get()->getBindingDataType(b)
                         << std::endl;
            mInOut["output"] = mEngine.get()->getBindingName(b);
        }
    }
}

//!
//! \brief Populate per tensor dyanamic range values
//!
bool sampleINT8API::readPerTensorDynamicRangeValues()
{
    std::ifstream iDynamicRangeStream(mParams.dynamicRangeFileName);
    if (!iDynamicRangeStream)
    {
        gLogError << "Could not find per tensor scales file: " << mParams.dynamicRangeFileName << std::endl;
        return false;
    }

    std::string line;
    char delim = ':';
    while (std::getline(iDynamicRangeStream, line))
    {
        std::istringstream iline(line);
        std::string token;
        std::getline(iline, token, delim);
        std::string tensorName = token;
        std::getline(iline, token, delim);
        float dynamicRange = std::stof(token);
        mPerTensorDynamicRangeMap[tensorName] = dynamicRange;
    }
    return true;
}

//!
//! \brief  Sets computation precision for network layers
//!
void sampleINT8API::setLayerPrecision(SampleUniquePtr<nvinfer1::INetworkDefinition>& network)
{
    gLogInfo << "Setting Per Layer Computation Precision" << std::endl;
    for (int i = 0; i < network->getNbLayers(); ++i)
    {
        auto layer = network->getLayer(i);
        if (mParams.verbose)
        {
            std::string layerName = layer->getName();
            gLogInfo << "Layer: " << layerName << ". Precision: INT8" << std::endl;
        }
        // set computation precision of the layer
        layer->setPrecision(nvinfer1::DataType::kINT8);

        for (int j = 0; j < layer->getNbOutputs(); ++j)
        {
            std::string tensorName = layer->getOutput(j)->getName();
            if (mParams.verbose)
            {
                std::string tensorName = layer->getOutput(j)->getName();
                gLogInfo << "Tensor: " << tensorName << ". OutputType: INT8" << std::endl;
            }
            // set output type of the tensor
            layer->setOutputType(j, nvinfer1::DataType::kINT8);
        }
    }
}

//!
//! \brief  Write network tensor names to a file.
//!
void sampleINT8API::writeNetworkTensorNames(const SampleUniquePtr<nvinfer1::INetworkDefinition>& network)
{
    gLogInfo << "Sample requires to run with per tensor dynamic range." << std::endl;
    gLogInfo << "In order to run Int8 inference without calibration, user will need to provide dynamic range for all the network tensors." << std::endl;

    std::ofstream tensorsFile{mParams.networkTensorsFileName};

    // Iterate through network inputs to write names of input tensors.
    for (int i = 0; i < network->getNbInputs(); ++i)
    {
        string tName = network->getInput(i)->getName();
        tensorsFile << "TensorName: " << tName << std::endl;
        if (mParams.verbose)
        {
            gLogInfo << "TensorName: " << tName << std::endl;
        }
    }

    // Iterate through network layers.
    for (int i = 0; i < network->getNbLayers(); ++i)
    {
        // Write output tensors of a layer to the file.
        for (int j = 0; j < network->getLayer(i)->getNbOutputs(); ++j)
        {
            string tName = network->getLayer(i)->getOutput(j)->getName();
            tensorsFile << "TensorName: " << tName << std::endl;
            if (mParams.verbose)
            {
                gLogInfo << "TensorName: " << tName << std::endl;
            }
        }
    }
    tensorsFile.close();
    gLogInfo << "Successfully generated network tensor names. Writing: " << mParams.networkTensorsFileName << std::endl;
    gLogInfo << "Use the generated tensor names file to create dynamic range file for Int8 inference. Follow README.md for instructions to generate dynamic_ranges.txt file." << std::endl;
}

//!
//! \brief  Sets custom dynamic range for network tensors
//!
bool sampleINT8API::setDynamicRange(SampleUniquePtr<nvinfer1::INetworkDefinition>& network)
{
    // populate per tensor dynamic range
    if (!readPerTensorDynamicRangeValues())
    {
        return false;
    }

    gLogInfo << "Setting Per Tensor Dynamic Range" << std::endl;
    if (mParams.verbose)
    {
        gLogInfo << "If dynamic range for a tensor is missing, TensorRT will run inference assuming dynamic range for the tensor as optional." << std::endl;
        gLogInfo << "If dynamic range for a tensor is required then inference will fail. Follow README.md to generate missing per tensor dynamic range." << std::endl;
    }

    // set dynamic range for network input tensors
    for (int i = 0; i < network->getNbInputs(); ++i)
    {
        string tName = network->getInput(i)->getName();
        if (mPerTensorDynamicRangeMap.find(tName) != mPerTensorDynamicRangeMap.end())
        {
            network->getInput(i)->setDynamicRange(-mPerTensorDynamicRangeMap.at(tName), mPerTensorDynamicRangeMap.at(tName));
        }
        else
        {
            if (mParams.verbose)
            {
                gLogWarning << "Missing dynamic range for tensor: " << tName << std::endl;
            }
        }
    }

    // set dynamic range for layer output tensors
    for (int i = 0; i < network->getNbLayers(); ++i)
    {
        for (int j = 0; j < network->getLayer(i)->getNbOutputs(); ++j)
        {
            string tName = network->getLayer(i)->getOutput(j)->getName();
            if (mPerTensorDynamicRangeMap.find(tName) != mPerTensorDynamicRangeMap.end())
            {
                // Calibrator generated dynamic range for network tensor can be overriden or set using below API
                network->getLayer(i)->getOutput(j)->setDynamicRange(-mPerTensorDynamicRangeMap.at(tName), mPerTensorDynamicRangeMap.at(tName));
            }
            else
            {
                if (mParams.verbose)
                {
                    gLogWarning << "Missing dynamic range for tensor: " << tName << std::endl;
                }
            }
        }
    }

    if (mParams.verbose)
    {
        gLogInfo << "Per Tensor Dynamic Range Values for the Network:" << std::endl;
        for (auto iter = mPerTensorDynamicRangeMap.begin(); iter != mPerTensorDynamicRangeMap.end(); ++iter)
            gLogInfo << "Tensor: " << iter->first << ". Max Absolute Dynamic Range: " << iter->second << std::endl;
    }
    return true;
}

//!
//! \brief Preprocess inputs and allocate host/device input buffers
//!
bool sampleINT8API::prepareInput(const samplesCommon::BufferManager& buffers)
{
    if (samplesCommon::toLower(samplesCommon::getFileType(mParams.imageFileName)).compare("ppm") != 0)
    {
        gLogError << "Wrong format: " << mParams.imageFileName << " is not a ppm file." << std::endl;
        return false;
    }

    // Prepare PPM Buffer to read the input image
    samplesCommon::PPM<kINPUT_C, kINPUT_H, kINPUT_W> ppm;
    samplesCommon::readPPMFile(mParams.imageFileName, ppm);

    float* hostInputBuffer = static_cast<float*>(buffers.getHostBuffer(mInOut["input"]));

    // Convert HWC to CHW and Normalize
    for (int c = 0; c < kINPUT_C; ++c)
    {
        for (int h = 0; h < kINPUT_H; ++h)
        {
            for (int w = 0; w < kINPUT_W; ++w)
            {
                int dstIdx = c * kINPUT_H * kINPUT_W + h * kINPUT_W + w;
                int srcIdx = h * kINPUT_W * kINPUT_C + w * kINPUT_C + c;
                // This equation include 3 steps
                // 1. Scale Image to range [0.f, 1.0f]
                // 2. Normalize Image using per channel Mean and per channel Standard Deviation
                // 3. Shuffle HWC to CHW form
                hostInputBuffer[dstIdx] = (float(ppm.buffer[srcIdx]) / kScale - kMean[c]) / kStdDev[c];
            }
        }
    }
    return true;
}

//!
//! \brief Verifies that the output is correct and prints it
//!
bool sampleINT8API::verifyOutput(const samplesCommon::BufferManager& buffers) const
{
    // copy output host buffer data for further processing
    const float* probPtr = static_cast<const float*>(buffers.getHostBuffer(mInOut.at("output")));
    vector<float> output(probPtr, probPtr + mOutputDims.d[0] * mParams.batchSize);

    auto inds = samplesCommon::argsort(output.cbegin(), output.cend(), true);

    // read reference lables to generate prediction lables
    vector<string> referenceVector;
    if (!samplesCommon::readReferenceFile(mParams.referenceFileName, referenceVector))
    {
        gLogError << "Unable to read reference file: " << mParams.referenceFileName << std::endl;
        return false;
    }

    vector<string> top5Result = samplesCommon::classify(referenceVector, output, 5);

    gLogInfo << "sampleINT8API result: Detected:" << std::endl;
    for (int i = 1; i <= 5; ++i)
        gLogInfo << "[" << i << "]  " << top5Result[i - 1] << std::endl;

    return true;
}

//!
//! \brief This function creates the network, configures the builder and creates the network engine
//!
//! \details This function creates INT8 classification network by parsing the onnx model and builds
//!          the engine that will be used to run INT8 inference (mEngine)
//!
//! \return Returns true if the engine was created successfully and false otherwise
//!
Logger::TestResult sampleINT8API::build()
{
    auto builder = SampleUniquePtr<nvinfer1::IBuilder>(nvinfer1::createInferBuilder(gLogger.getTRTLogger()));
    if (!builder)
    {
        gLogError << "Unable to create builder object." << std::endl;
        return Logger::TestResult::kFAILED;
    }

    auto network = SampleUniquePtr<nvinfer1::INetworkDefinition>(builder->createNetwork());
    if (!network)
    {
        gLogError << "Unable to create network object." << mParams.referenceFileName << std::endl;
        return Logger::TestResult::kFAILED;
    }

    auto parser = SampleUniquePtr<nvonnxparser::IParser>(nvonnxparser::createParser(*network, gLogger.getTRTLogger()));
    if (!parser)
    {
        gLogError << "Unable to create parser object." << mParams.referenceFileName << std::endl;
        return Logger::TestResult::kFAILED;
    }

    // Parse ONNX model file to populate TensorRT INetwork
    int verbosity = (int) nvinfer1::ILogger::Severity::kERROR;
    if (!parser->parseFromFile(mParams.modelFileName.c_str(), verbosity))
    {
        gLogError << "Unable to parse ONNX model file: " << mParams.modelFileName << std::endl;
        return Logger::TestResult::kFAILED;
    }

    if (mParams.writeNetworkTensors)
    {
        writeNetworkTensorNames(network);
        return Logger::TestResult::kFAILED;
    }

    if (!builder->platformHasFastInt8())
    {
        gLogError << "Platform does not support INT8 inference. sampleINT8API can only run in INT8 Mode." << std::endl;
        return Logger::TestResult::kWAIVED;
    }

    // Configure buider
    builder->allowGPUFallback(true);
    builder->setMaxWorkspaceSize(1_GB);

    // Enable INT8 model. Required to set custom per tensor dynamic range or INT8 Calibration
    builder->setInt8Mode(true);
    // Mark calibrator as null. As user provides dynamic range for each tensor, no calibrator is required
    builder->setInt8Calibrator(nullptr);

    auto maxBatchSize = mParams.batchSize;
    if (mParams.dlaCore >= 0)
    {
        samplesCommon::enableDLA(builder.get(), mParams.dlaCore);
        if (maxBatchSize > builder->getMaxDLABatchSize())
        {
            std::cerr << "Requested batch size " << maxBatchSize << " is greater than the max DLA batch size of "
                      << builder->getMaxDLABatchSize() << ". Reducing batch size accordingly." << std::endl;
            maxBatchSize = builder->getMaxDLABatchSize();
        }
    }
    builder->setMaxBatchSize(maxBatchSize);

    // force layer to execute with required precision
    builder->setStrictTypeConstraints(true);
    setLayerPrecision(network);

    // set INT8 Per Tensor Dynamic range
    if (!setDynamicRange(network))
    {
        gLogError << "Unable to set per tensor dynamic range." << std::endl;
        return Logger::TestResult::kFAILED;
    }

    // build TRT engine
    mEngine = std::shared_ptr<nvinfer1::ICudaEngine>(builder->buildCudaEngine(*network), samplesCommon::InferDeleter());
    if (!mEngine)
    {
        gLogError << "Unable to build cuda engine." << std::endl;
        return Logger::TestResult::kFAILED;
    }

    // populates input output map structure
    getInputOutputNames();

    // derive input/output dims from engine bindings
    const int inputIndex = mEngine.get()->getBindingIndex(mInOut["input"].c_str());
    mInputDims = mEngine.get()->getBindingDimensions(inputIndex);

    const int outputIndex = mEngine.get()->getBindingIndex(mInOut["output"].c_str());
    mOutputDims = mEngine.get()->getBindingDimensions(outputIndex);

    return Logger::TestResult::kRUNNING;
}

//!
//! \brief This function runs the TensorRT inference engine for this sample
//!
//! \details This function is the main execution function of the sample. It allocates
//!          the buffer, sets inputs, executes the engine, and verifies the output
//!
Logger::TestResult sampleINT8API::infer()
{
    // Create RAII buffer manager object
    samplesCommon::BufferManager buffers(mEngine, mParams.batchSize);

    auto context = SampleUniquePtr<nvinfer1::IExecutionContext>(mEngine->createExecutionContext());
    if (!context)
    {
        return Logger::TestResult::kFAILED;
    }

    // Read the input data into the managed buffers
    // There should be just 1 input tensor

    if (!prepareInput(buffers))
    {
        return Logger::TestResult::kFAILED;
    }

    // Create CUDA stream for the execution of this inference
    cudaStream_t stream;
    CHECK(cudaStreamCreate(&stream));

    // Asynchronously copy data from host input buffers to device input buffers
    buffers.copyInputToDeviceAsync(stream);

    // Asynchronously enqueue the inference work
    if (!context->enqueue(mParams.batchSize, buffers.getDeviceBindings().data(), stream, nullptr))
    {
        return Logger::TestResult::kFAILED;
    }

    // Asynchronously copy data from device output buffers to host output buffers
    buffers.copyOutputToHostAsync(stream);

    // Wait for the work in the stream to complete
    cudaStreamSynchronize(stream);

    // Release stream
    cudaStreamDestroy(stream);

    // Check and print the output of the inference
    bool outputCorrect = false;
    outputCorrect = verifyOutput(buffers);

    return outputCorrect ? Logger::TestResult::kRUNNING : Logger::TestResult::kFAILED;
}

//!
//! \brief This function can be used to clean up any state created in the sample class
//!
Logger::TestResult sampleINT8API::teardown()
{
    //! Clean up the libprotobuf files as the parsing is complete
    //! \note It is not safe to use any other part of the protocol buffers library after
    //! ShutdownProtobufLibrary() has been called.
    return Logger::TestResult::kRUNNING;
}

int main(int argc, char** argv)
{
    SampleINT8APIArgs args;
    bool argsOK = parseSampleINT8APIArgs(args, argc, argv);

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
    if (args.verbose)
    {
        gLogger.setReportableSeverity(nvinfer1::ILogger::Severity::kVERBOSE);
    }

    auto sampleTest = gLogger.defineTest(gSampleName, argc, const_cast<const char**>(argv));

    gLogger.reportTestStart(sampleTest);

    SampleINT8APIParams params;
    initializeSampleParams(args, params);

    sampleINT8API sample(params);
    gLogInfo << "Building and running a INT8 GPU inference engine for " << params.modelFileName << std::endl;

    auto buildStatus = sample.build();
    if (buildStatus == Logger::TestResult::kWAIVED)
    {
        return gLogger.reportWaive(sampleTest);
    }
    else if (buildStatus == Logger::TestResult::kFAILED)
    {
        return gLogger.reportFail(sampleTest);
    }

    if (sample.infer() != Logger::TestResult::kRUNNING)
    {
        return gLogger.reportFail(sampleTest);
    }

    if (sample.teardown() != Logger::TestResult::kRUNNING)
    {
        return gLogger.reportFail(sampleTest);
    }

    return gLogger.reportPass(sampleTest);
}
