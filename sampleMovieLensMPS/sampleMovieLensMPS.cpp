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

#include <cmath>
#include <ctime>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <sys/stat.h>
#include <vector>
#include <algorithm>
#include <iomanip>
#include <atomic>
#include <cuda_runtime_api.h>
#include <cuda_profiler_api.h>

// Required to enable MPS Support
#include <stdio.h>
#include <semaphore.h>
#include <fcntl.h>

#ifndef _MSC_VER
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <sched.h>
#endif

#include "NvInfer.h"
#include "NvUffParser.h"
#include "logger.h"
#include "common.h"

using namespace nvinfer1;
using namespace nvuffparser;

const std::string gSampleName = "TensorRT.sample_movielens_mps";

// constants that are known about the MovieLens (NCF) MLP network.
static const int32_t NUM_USERS{32};          // Total number of users.
static const int32_t TOPK_MOVIES{1};         // The output of the topK layer for MovieLens sample.
static const int32_t NUM_INDICES{100};       // Total numbers of Movies to predict per user.
static const int32_t EMBEDDING_VEC_SIZE{32}; // Embedding vector size of each user and item.
static const int32_t THREADS{1};
static const char* USER_BLOB_NAME{"user_input"};                // user input blob name.
static const char* ITEM_BLOB_NAME{"item_input"};                // item input blob name.
static const char* TOPK_ITEM_PROB{"topk_values"};               // predicted item probability blob name.
static const char* TOPK_ITEM_NAME{"topk_items"};                // predicted item probability blob name.
static const char* RATING_INPUT_FILE{"movielens_ratings.txt"};  // The default input file with 50 users and groundtruth data.
static const char* DEFAULT_WEIGHT_FILE{"sampleMovieLens.wts2"}; // The weight file produced from README.txt
static const char* UFF_MODEL_FILE{"sampleMovieLens.uff"};
static const char* UFF_OUTPUT_NODE{"prediction/Sigmoid"};
static const char* ENGINE_FILE{"sampleMovieLens.engine"};
static const int32_t DEVICE{0};
static const std::vector<std::string> directories{"data/samples/movielens/", "data/movielens/"};

template <typename T>
using SampleUniquePtr = std::unique_ptr<T, samplesCommon::InferDeleter>;

class Semaphore
{
public:
    Semaphore(const char* semName) : mSemName(semName)
    {
    }

    ~Semaphore()
    {
        sem_unlink(mSemName);
        sem_close(mSemEngine);
    }

    void wait()
    {
        sem_wait(mSemEngine);
    }

    void post()
    {
        sem_post(mSemEngine);
    }

    void open()
    {
        mSemEngine = sem_open(mSemName, O_CREAT | O_EXCL, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP, 0);
        if (mSemEngine == SEM_FAILED)
        {
            throw std::runtime_error("Could not create semaphore");
        }
    }
private:
    const char* mSemName;
    sem_t* mSemEngine;
};

class SharedMemory
{
public:
    SharedMemory(const char* modelStreamFd) : mModelStreamFd(modelStreamFd)
    {
    }

    ~SharedMemory()
    {
        shm_unlink(mModelStreamFd);
    }

    int open_ro()
    {
        return open(O_RDONLY, 0666);
    }

    int open_rw()
    {
        return open(O_RDWR | O_CREAT, 0666);
    }
private:
    int open(int flag, mode_t mode)
    {
        int fd = shm_open(mModelStreamFd, flag, mode);
        if (fd <= 0)
        {
            throw std::runtime_error("Could not create file descriptor: /dev/shm" + std::string(mModelStreamFd));
        }
        return fd;
    }
    const char* mModelStreamFd;
};

// The OutptutArgs struct holds intermediate/final outputs generated by the MovieLens structure per user.
struct OutputArgs
{
    int32_t userId;                                         // The user Id per batch.
    int32_t expectedPredictedMaxRatingItem;                 // The Expected Max Rating Item per user (inference ground truth).
    float expectedPredictedMaxRatingItemProb;               // The Expected Max Rating Probability. (inference ground truth).
    std::vector<int32_t> allItems;                          // All inferred items per user.
    std::vector<std::pair<int32_t, float>> itemProbPairVec; // Expected topK items and prob per user.
};                                                          // struct pargs

struct Args
{
    int32_t embeddingVecSize{EMBEDDING_VEC_SIZE};
    int32_t numUsers{NUM_USERS};                    // Total number of users. Should be equal to ratings file users count.
    int32_t topKMovies{TOPK_MOVIES};                // TopK movies per user.
    int32_t numMoviesPerUser{NUM_INDICES};          // The number of movies per user.
    int32_t nbProcesses{THREADS};                   // Number of concurrent processes
    std::string weightFile{DEFAULT_WEIGHT_FILE};    // Weight file (.wts2) format Movielens sample.
    std::string ratingInputFile{RATING_INPUT_FILE}; // The input rating file.
    std::string uffFile{UFF_MODEL_FILE};
    std::string engineFile{ENGINE_FILE};
    bool enableFP16{false};    // Enable ability to run in FP16 mode.
    bool enableInt8{false};    // Enable ability to run in Int8 mode.
    bool enableVerbose{false}; // Set reportable severity of logger to kVERBOSE.
    bool help{false};          // Print help info.
    int useDLACore{-1};
    // The below structures are used to compare the predicted values to inference (ground truth)
    std::map<int32_t, std::vector<int32_t>> userToItemsMap;                              // Lookup for inferred items for each user.
    std::map<int32_t, std::vector<std::pair<int32_t, float>>> userToExpectedItemProbMap; // Lookup for topK items and probs for each user.
    int32_t device{DEVICE};
    std::vector<OutputArgs> pargsVec;
    std::atomic<int32_t> failCount;                         // Number threads that failed inference.
}; // struct args

struct Batch
{
    Batch(ICudaEngine* engine, void* userInputPtr, void* itemInputPtr, const Args& args)
    {
        mEngine = engine;
        mContext = SampleUniquePtr<IExecutionContext>(mEngine->createExecutionContext());

        CHECK(cudaStreamCreate(&mStream));

        // In order to bind the buffers, we need to know the names of the input and output tensors.
        // note that indices are guaranteed to be less than IEngine::getNbBindings()
        int userInputIndex = mEngine->getBindingIndex(USER_BLOB_NAME);
        int itemInputIndex = mEngine->getBindingIndex(ITEM_BLOB_NAME);
        int outputPredictionIndex = mEngine->getBindingIndex(UFF_OUTPUT_NODE);
        int outputItemProbIndex = mEngine->getBindingIndex(TOPK_ITEM_PROB);
        int outputItemNameIndex = mEngine->getBindingIndex(TOPK_ITEM_NAME);

        mMemSizes.push_back(args.numUsers * args.numMoviesPerUser * sizeof(float));
        mMemSizes.push_back(args.numUsers * args.numMoviesPerUser * sizeof(float));
        mMemSizes.push_back(args.numUsers * args.numMoviesPerUser * sizeof(float));
        mMemSizes.push_back(args.numUsers * args.topKMovies * sizeof(float));
        mMemSizes.push_back(args.numUsers * args.topKMovies * sizeof(float));

        CHECK(cudaMallocHost(&mHostMemory[userInputIndex], mMemSizes[userInputIndex]));
        CHECK(cudaMallocHost(&mHostMemory[itemInputIndex], mMemSizes[itemInputIndex]));
        CHECK(cudaMallocHost(&mHostMemory[outputPredictionIndex], mMemSizes[outputPredictionIndex]));
        CHECK(cudaMallocHost(&mHostMemory[outputItemProbIndex], mMemSizes[outputItemProbIndex]));
        CHECK(cudaMallocHost(&mHostMemory[outputItemNameIndex], mMemSizes[outputItemNameIndex]));

        // copy the data to host memory
        for (unsigned int i = 0; i < (mMemSizes[userInputIndex]) / sizeof(float); ++i)
        {
            *(static_cast<uint32_t*>(mHostMemory[userInputIndex]) + i) = *((uint32_t*) userInputPtr + i);
        }
        for (unsigned int i = 0; i < (mMemSizes[itemInputIndex]) / sizeof(float); ++i)
        {
            *(static_cast<uint32_t*>(mHostMemory[itemInputIndex]) + i) = *((uint32_t*) itemInputPtr + i);
        }

        // allocate GPU memory
        CHECK(cudaMalloc(&mDeviceMemory[userInputIndex], mMemSizes[userInputIndex]));
        CHECK(cudaMalloc(&mDeviceMemory[itemInputIndex], mMemSizes[itemInputIndex]));
        CHECK(cudaMalloc(&mDeviceMemory[outputPredictionIndex], mMemSizes[outputPredictionIndex]));
        CHECK(cudaMalloc(&mDeviceMemory[outputItemProbIndex], mMemSizes[outputItemProbIndex]));
        CHECK(cudaMalloc(&mDeviceMemory[outputItemNameIndex], mMemSizes[outputItemNameIndex]));
    }

    ~Batch()
    {
        for (auto p : mHostMemory)
            CHECK(cudaFreeHost(p));
        for (auto p : mDeviceMemory)
            CHECK(cudaFree(p));
        CHECK(cudaStreamDestroy(mStream));
    }

    ICudaEngine* mEngine;
    SampleUniquePtr<nvinfer1::IExecutionContext> mContext;
    cudaStream_t mStream;
    void* mHostMemory[5];
    void* mDeviceMemory[5];
    std::vector<size_t> mMemSizes;
};

void printHelpInfo()
{
    std::cout << "Usage:\n"
              << "\t ./sample_movielens_mps [-h] [-b NUM_USERS] [-p NUM_PROCESSES] [--useDLACore=<int>] [--verbose]\n"
              << "\t-h             Display help information. All single dash options enable perf mode.\n"
              << "\t-b             Number of Users i.e. Batch Size (default numUsers=32).\n"
              << "\t-p             Number of child processes to launch (default nbProcesses=1. Using MPS with this option is strongly recommended).\n"
              << "\t--useDLACore=N Specify a DLA engine for layers that support DLA. Value can range from 0 to n-1, where n is the number of DLA engines on the platform.\n"
              << "\t--verbose      Enable verbose prints.\n"
              << "\t--int8         Run in Int8 mode.\n"
              << "\t--fp16         Run in FP16 mode.\n"
              << std::endl;
}

// Parse the arguments and return failure if arguments are incorrect
bool parseArgs(Args& args, int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        std::string argStr(argv[i]);

        if (argStr == "-h")
        {
            args.help = true;
            return true;
        }
        if (argStr == "-b")
        {
            i++;
            args.numUsers = std::atoi(argv[i]);
        }
        else if (argStr == "-p")
        {
            i++;
            args.nbProcesses = std::atoi(argv[i]);
        }
        else if (argStr == "--verbose")
        {
            args.enableVerbose = true;
            setReportableSeverity(Severity::kVERBOSE);
        }
        else if (argStr.compare(0, 13, "--useDLACore=") == 0 && argStr.size() > 13)
        {
            args.useDLACore = stoi(argv[i] + 13);
        }
        else if (argStr == "--int8")
        {
            args.enableInt8 = true;
        }
        else if (argStr == "--fp16")
        {
            args.enableFP16 = true;
        }
        else
        {
            return false;
        }
    }
    return true;
}

void printOutputArgs(OutputArgs& pargs)
{
    gLogVerbose << "User Id                            :   " << pargs.userId << std::endl;
    gLogVerbose << "Expected Predicted Max Rating Item :   " << pargs.expectedPredictedMaxRatingItem << std::endl;
    gLogVerbose << "Expected Predicted Max Rating Prob :   " << pargs.expectedPredictedMaxRatingItemProb << std::endl;
    gLogVerbose << "Total TopK Items : " << pargs.itemProbPairVec.size() << std::endl;
    for (unsigned i = 0; i < pargs.itemProbPairVec.size(); ++i)
        gLogVerbose << pargs.itemProbPairVec.at(i).first << " : " << pargs.itemProbPairVec.at(i).second << std::endl;
}

std::string readNextLine(ifstream& file, char delim)
{
    std::string line;
    std::getline(file, line);
    auto pos = line.find(delim);
    line = line.substr(pos + 1);
    return line;
}

void readInputSample(ifstream& file, OutputArgs& pargs, std::string line, const Args& args)
{
    // read user name
    char delim = ':';
    auto pos = line.find(delim);
    line = line.substr(pos + 1);
    pargs.userId = std::stoi(line);
    // read items
    std::string items = readNextLine(file, delim);
    items = items.substr(2, items.size() - 2);
    std::stringstream ss(items);
    std::string i;
    while (ss >> i)
    {
        if (ss.peek() == ',' || ss.peek() == ' ')
            ss.ignore();
        i = i.substr(0, i.size() - 1);
        pargs.allItems.push_back(stoi(i));
    }

    // read expected predicted max rating item
    pargs.expectedPredictedMaxRatingItem = std::stoi(readNextLine(file, delim));

    // read expected predicted max rating prob
    std::string prob = readNextLine(file, delim);
    prob = prob.substr(2, prob.size() - 3);
    pargs.expectedPredictedMaxRatingItemProb = std::stof(prob);

    // skip line
    std::getline(file, line);
    std::getline(file, line);

    // read all the top 10 prediction ratings
    for (int i = 0; i < 10; ++i)
    {
        auto pos = line.find(delim);
        int32_t item = std::stoi(line.substr(0, pos - 1));
        float prob = std::stof(line.substr(pos + 2));
        pargs.itemProbPairVec.emplace_back((make_pair(item, prob)));
        std::getline(file, line);
    }
}

void parseMovieLensData(Args& args)
{
    std::ifstream file;
    file.open(args.ratingInputFile);
    std::string line;
    int userIdx = 0;
    while (std::getline(file, line) && userIdx < args.numUsers)
    {
        OutputArgs pargs;
        readInputSample(file, pargs, line, args);

        // store the pargs in the global data structure. Hack.
        args.pargsVec.push_back(pargs);

        args.userToItemsMap[userIdx] = std::move(pargs.allItems);
        args.userToExpectedItemProbMap[userIdx] = std::move(pargs.itemProbPairVec);

        userIdx++;
        printOutputArgs(pargs);
    }

    // number of users should be equal to number of users in rating file
    if (args.numUsers != userIdx)
    {
        throw std::runtime_error("Invalid ratings file.");
    }
}

template <typename T1, typename T2>
bool printInferenceOutput(void* userInputPtr, void* itemInputPtr, void* topKItemNumberPtr, void* topKItemProbPtr, const Args& args)
{
    bool pass{true};
    T1* userInput{static_cast<T1*>(userInputPtr)};
    T1* topKItemNumber{static_cast<T1*>(topKItemNumberPtr)};
    T2* topKItemProb{static_cast<T2*>(topKItemProbPtr)};

    gLogInfo << "Num of users : " << args.numUsers << std::endl;
    gLogInfo << "Num of Movies : " << args.numMoviesPerUser << std::endl;

    gLogVerbose << "|-----------|------------|-----------------|-----------------|" << std::endl;
    gLogVerbose << "|   User    |   Item     |  Expected Prob  |  Predicted Prob |" << std::endl;
    gLogVerbose << "|-----------|------------|-----------------|-----------------|" << std::endl;

    for (int i = 0; i < args.numUsers; ++i)
    {
        int userIdx = userInput[i * args.numMoviesPerUser];
        int maxPredictedIdx = topKItemNumber[i * args.topKMovies];
        int maxExpectedItem = args.userToExpectedItemProbMap.at(userIdx).at(0).first;
        int maxPredictedItem = args.userToItemsMap.at(userIdx).at(maxPredictedIdx);
        pass &= (maxExpectedItem == maxPredictedItem);

        for (int k = 0; k < args.topKMovies; ++k)
        {
            int predictedIdx = topKItemNumber[i * args.topKMovies + k];
            float predictedProb = topKItemProb[i * args.topKMovies + k];
            float expectedProb = args.userToExpectedItemProbMap.at(userIdx).at(k).second;
            int predictedItem = args.userToItemsMap.at(userIdx).at(predictedIdx);
            gLogVerbose << "|" << setw(10) << userIdx << " | " << setw(10) << predictedItem << " | " << setw(15) << expectedProb << " | " << setw(15) << predictedProb << " | " << std::endl;
        }
    }

    for (int i = 0; i < args.numUsers; ++i)
    {
        int userIdx = userInput[i * args.numMoviesPerUser];
        int maxPredictedIdx = topKItemNumber[i * args.topKMovies];
        int maxExpectedItem = args.userToExpectedItemProbMap.at(userIdx).at(0).first;
        int maxPredictedItem = args.userToItemsMap.at(userIdx).at(maxPredictedIdx);
        gLogInfo << "| PID : " << setw(4) << getpid() << " | User :" << setw(4) << userIdx << "  |  Expected Item :" << setw(5) << maxExpectedItem << "  |  Predicted Item :" << setw(5) << maxPredictedItem << " | " << std::endl;
    }

    return pass;
}

void submitWork(Batch& b, const Args& args)
{
    int userInputIndex = b.mEngine->getBindingIndex(USER_BLOB_NAME);
    int itemInputIndex = b.mEngine->getBindingIndex(ITEM_BLOB_NAME);
    int outputPredictionIndex = b.mEngine->getBindingIndex(UFF_OUTPUT_NODE);
    int outputItemProbIndex = b.mEngine->getBindingIndex(TOPK_ITEM_PROB);
    int outputItemNameIndex = b.mEngine->getBindingIndex(TOPK_ITEM_NAME);

    // Copy input from host to device
    CHECK(cudaMemcpyAsync(b.mDeviceMemory[userInputIndex], b.mHostMemory[userInputIndex], b.mMemSizes[userInputIndex], cudaMemcpyHostToDevice, b.mStream));
    CHECK(cudaMemcpyAsync(b.mDeviceMemory[itemInputIndex], b.mHostMemory[itemInputIndex], b.mMemSizes[itemInputIndex], cudaMemcpyHostToDevice, b.mStream));

    b.mContext->enqueue(args.numUsers, b.mDeviceMemory, b.mStream, nullptr);

    // copy output from device to host
    CHECK(cudaMemcpyAsync(b.mHostMemory[outputPredictionIndex], b.mDeviceMemory[outputPredictionIndex], b.mMemSizes[outputPredictionIndex], cudaMemcpyDeviceToHost, b.mStream));
    CHECK(cudaMemcpyAsync(b.mHostMemory[outputItemProbIndex], b.mDeviceMemory[outputItemProbIndex], b.mMemSizes[outputItemProbIndex], cudaMemcpyDeviceToHost, b.mStream));
    CHECK(cudaMemcpyAsync(b.mHostMemory[outputItemNameIndex], b.mDeviceMemory[outputItemNameIndex], b.mMemSizes[outputItemNameIndex], cudaMemcpyDeviceToHost, b.mStream));
}

shared_ptr<ICudaEngine> loadModelAndCreateEngine(const char* uffFile, IUffParser* parser, const Args& args)
{
    // Create the builder
    auto builder = SampleUniquePtr<nvinfer1::IBuilder>(nvinfer1::createInferBuilder(gLogger.getTRTLogger()));
    if (builder == nullptr)
    {
        throw std::runtime_error("Could not create builder.");
    }

    auto network = SampleUniquePtr<nvinfer1::INetworkDefinition>(builder->createNetwork());
    if (network == nullptr)
    {
        throw std::runtime_error("Could not create network.");
    }

    gLogInfo << "Begin parsing model..." << std::endl;

    auto dType = args.enableFP16 ? nvinfer1::DataType::kHALF : nvinfer1::DataType::kFLOAT;

    // Parse the uff model to populate the network
    if (!parser->parse(uffFile, *network, dType))
    {
        gLogError << "Failure while parsing UFF file" << std::endl;
        return nullptr;
    }

    gLogInfo << "End parsing model..." << std::endl;

    // Add postprocessing i.e. topk layer to the UFF Network
    // Retrieve last layer of UFF Network
    auto uffLastLayer = network->getLayer(network->getNbLayers() - 1);

    // Reshape output of fully connected layer numOfMovies x 1 x 1 x 1 to numOfMovies x 1 x 1.
    auto reshapeLayer = network->addShuffle(*uffLastLayer->getOutput(0));
    reshapeLayer->setReshapeDimensions(Dims3{1, args.numMoviesPerUser, 1});
    if (reshapeLayer == nullptr)
    {
        throw std::runtime_error("Could not create reshape layer.");
    }

    // Apply TopK layer to retrieve item probabilities and corresponding index number.
    auto topK = network->addTopK(*reshapeLayer->getOutput(0), TopKOperation::kMAX, args.topKMovies, 0x2);
    if (topK == nullptr)
    {
        throw std::runtime_error("Could not create TopK layer.");
    }

    // Mark outputs for index and probs. Also need to set the item layer type == kINT32.
    topK->getOutput(0)->setName(TOPK_ITEM_PROB);
    topK->getOutput(1)->setName(TOPK_ITEM_NAME);

    // Specify topK tensors as outputs
    network->markOutput(*topK->getOutput(0));
    network->markOutput(*topK->getOutput(1));

    // Set the topK indices tensor as INT32 type
    topK->getOutput(1)->setType(DataType::kINT32);

    // Build the engine
    builder->setMaxBatchSize(args.numUsers);
    builder->setMaxWorkspaceSize(1_GB); // The _GB literal operator is defined in common.h
    builder->setFp16Mode(args.enableFP16);
    builder->setInt8Mode(args.enableInt8);

    samplesCommon::setDummyInt8Scales(builder.get(), network.get());
    samplesCommon::enableDLA(builder.get(), args.useDLACore);

    auto engine =  samplesCommon::infer_object(builder->buildCudaEngine(*network));
    if (!engine)
    {
        gLogError << "Unable to create engine" << std::endl;
        return nullptr;
    }

    gLogInfo << "End building engine..." << std::endl;
    return engine;
}

bool doInference(void* modelStreamData, int modelStreamSize, void* userInputPtr, void* itemInputPtr, Args& args)
{
    auto runtime = SampleUniquePtr<nvinfer1::IRuntime>(nvinfer1::createInferRuntime(gLogger.getTRTLogger()));
    if (args.useDLACore >= 0)
    {
        runtime->setDLACore(args.useDLACore);
    }

    auto engine = samplesCommon::infer_object(runtime->deserializeCudaEngine(modelStreamData, modelStreamSize, nullptr));

    Batch b{engine.get(), userInputPtr, itemInputPtr, args};

    {
        samplesCommon::GpuTimer timer{b.mStream};
        timer.start();
        // Run inference for all the nbProcesses
        submitWork(b, args);
        cudaStreamSynchronize(b.mStream);
        timer.stop();
        gLogInfo << "Done execution in process: " << getpid() << " . Duration : " << timer.microseconds() << " microseconds." << std::endl;
    }

    int outputItemProbIndex = b.mEngine->getBindingIndex(TOPK_ITEM_PROB);
    int outputItemNameIndex = b.mEngine->getBindingIndex(TOPK_ITEM_NAME);

    float* topKItemProb = static_cast<float*>(b.mHostMemory[outputItemProbIndex]);
    uint32_t* topKItemNumber = static_cast<uint32_t*>(b.mHostMemory[outputItemNameIndex]);
    bool pass{printInferenceOutput<uint32_t, float>(userInputPtr, itemInputPtr, topKItemNumber, topKItemProb, args)};

    return pass;
}

int mainMovieLensMPS(Args& args, OutputArgs& pargs)
{
    // Parse the ratings file and populate ground truth data
    args.ratingInputFile = locateFile(args.ratingInputFile, directories);
    gLogInfo << args.ratingInputFile << std::endl;

    // Parse ground truth data and inputs, common to all processes (if using MPS)
    parseMovieLensData(args);

    // Create uff parser
    args.uffFile = locateFile(args.uffFile, directories);
    auto parser = SampleUniquePtr<nvuffparser::IUffParser>(nvuffparser::createUffParser());

    // All nbProcesses should wait until the parent is done building the engine.
    Semaphore sem("/engine_built");
    sem.open();

    pid_t pid{};
    // Create child processes
    for (int i = 0; i < args.nbProcesses; ++i)
    {
        pid = fork();
        // Children should not create additional processes.
        if (pid == 0)
        {
            break;
        }
        else if (pid == -1)
        {
            throw std::runtime_error("Could not create child process");
        }
    }
    // Every process needs to know if it's a child or not.
    bool isParentProcess = (pid != 0);

    SharedMemory shm("/sampleMovieLens.modelStream");

    if (isParentProcess)
    {
        // Parent process should build an engine and write it to the shared buffer.
        Dims inputIndices;
        inputIndices.nbDims = 1;
        inputIndices.d[0] = args.numMoviesPerUser;

        parser->registerInput(USER_BLOB_NAME, inputIndices, UffInputOrder::kNCHW);
        parser->registerInput(ITEM_BLOB_NAME, inputIndices, UffInputOrder::kNCHW);
        parser->registerOutput(UFF_OUTPUT_NODE);

        auto engine = loadModelAndCreateEngine(args.uffFile.c_str(), parser.get(), args);
        if (engine.get() == nullptr)
        {
            throw std::runtime_error("Failed to create engine.");
        }

        auto modelStream = samplesCommon::infer_object(engine->serialize());

        size_t modelStreamSize = modelStream->size();
        // Create a shared buffer for the modelStream.
        int fd = shm.open_rw();

        fallocate(fd, 0, 0, modelStreamSize);
        void* modelStreamData = mmap(NULL, modelStreamSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        // Copy modelStream to the shared buffer.
        std::memcpy(modelStreamData, modelStream->data(), modelStreamSize);
        // Clean up.
        close(fd);
    }
    else
    {
        // Allocate input and output buffers on host.
        std::vector<uint32_t> userInput(args.numUsers * args.numMoviesPerUser * sizeof(float));
        std::vector<uint32_t> itemInput(args.numUsers * args.numMoviesPerUser * sizeof(float));

        for (int i = 0; i < args.numUsers; ++i)
        {
            for (int k = 0; k < args.numMoviesPerUser; ++k)
            {
                int idx = i * args.numMoviesPerUser + k;
                userInput[idx] = args.pargsVec[i].userId;
                itemInput[idx] = args.pargsVec[i].allItems.at(k);
            }
        }

        // Now wait for parent to construct engine and write the modelstream to the shared buffer.
        sem.wait();

        // Open a file descriptor for the shared buffer.
        int fd = shm.open_ro();

        // Get size of shared memory buffer.
        struct stat sb;
        fstat(fd, &sb);
        int modelStreamSize = sb.st_size;
        if (modelStreamSize <= 0)
        {
            throw std::runtime_error("Failed to fetch model stream from shared memory buffer.");
        }

        // Retrieve the modelStream and close the file descriptor.
        void* modelStreamData = mmap(NULL, modelStreamSize, PROT_READ, MAP_SHARED, fd, 0);
        close(fd);

        // All child processes will do inference and then exit.
        bool pass = doInference(modelStreamData, modelStreamSize, userInput.data(), itemInput.data(), args);
        exit(!pass);    // exit(0) when pass is true, exit(1) when pass is false
    }

    // Let children processes continue
    for (int j = 0; j < args.nbProcesses; ++j)
    {
        sem.post();
    }

    // Then time them.
    {
        samplesCommon::PreciseCpuTimer timer{};
        timer.start();
        int status;
        // Parent should wait for child processes.
        for (int i = 0; i < args.nbProcesses; ++i)
        {
            wait(&status);
            if (status) args.failCount++;
        }
        timer.stop();
        gLogInfo << "Number of processes executed: " << args.nbProcesses << ". Number of processes failed: " << args.failCount << "." << std::endl;
        gLogInfo << "Total MPS Run Duration: " << timer.milliseconds() << " milliseconds." << std::endl;
    }
    
    bool pass = !args.failCount;
    return pass;
}

int main(int argc, char* argv[])
{
    Args args;        // Global struct to store arguments
    OutputArgs pargs; // Ratings file struct

    // Parse arguments
    bool argsOK = parseArgs(args, argc, argv);
    args.failCount = 0;

    if (args.help)
    {
        printHelpInfo();
        return EXIT_SUCCESS;
    }

    if (!argsOK)
    {
        printHelpInfo();
        gLogError << "Invalid arguments" << std::endl;
        return EXIT_FAILURE;
    }

    auto sampleTest = gLogger.defineTest(gSampleName, argc, const_cast<const char**>(argv));
    gLogger.reportTestStart(sampleTest);
    bool pass = false;

    try
    {
        pass = mainMovieLensMPS(args, pargs);
    }
    catch (const std::exception& e)
    {
        gLogError << e.what() << std::endl;
    }
    return gLogger.reportTest(sampleTest, pass);
}
