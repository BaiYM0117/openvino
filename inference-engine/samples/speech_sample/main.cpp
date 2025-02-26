// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "speech_sample.hpp"

#include <gflags/gflags.h>
#include <functional>
#include <iostream>
#include <memory>
#include <map>
#include <fstream>
#include <random>
#include <string>
#include <vector>
#include <utility>
#include <time.h>
#include <thread>
#include <chrono>
#include <limits>
#include <iomanip>
#include <inference_engine.hpp>
#include <gna/gna_config.hpp>

#include <samples/common.hpp>
#include <samples/slog.hpp>
#include <samples/args_helper.hpp>

#define MAX_SCORE_DIFFERENCE 0.0001f
#define MAX_VAL_2B_FEAT 16384

using namespace InferenceEngine;

typedef std::chrono::high_resolution_clock Time;
typedef std::chrono::duration<double, std::ratio<1, 1000>> ms;
typedef std::chrono::duration<float> fsec;
typedef struct {
    uint32_t numScores;
    uint32_t numErrors;
    float threshold;
    float maxError;
    float rmsError;
    float sumError;
    float sumRmsError;
    float sumSquaredError;
    float maxRelError;
    float sumRelError;
    float sumSquaredRelError;
} score_error_t;

struct InferRequestStruct {
    InferRequest inferRequest;
    int frameIndex;
    uint32_t numFramesThisBatch;
};

void CheckNumberOfInputs(size_t numInputs, size_t numInputArkFiles) {
    if (numInputs != numInputArkFiles) {
        throw std::logic_error("Number of network inputs (" + std::to_string(numInputs) + ")"
                               " is not equal to number of ark files (" + std::to_string(numInputArkFiles) + ")");
    }
}

void GetKaldiArkInfo(const char *fileName,
                     uint32_t numArrayToFindSize,
                     uint32_t *ptrNumArrays,
                     uint32_t *ptrNumMemoryBytes) {
    uint32_t numArrays = 0;
    uint32_t numMemoryBytes = 0;

    std::ifstream in_file(fileName, std::ios::binary);
    if (in_file.good()) {
        while (!in_file.eof()) {
            std::string line;
            uint32_t numRows = 0u, numCols = 0u, num_bytes = 0u;
            std::getline(in_file, line, '\0');  // read variable length name followed by space and NUL
            std::getline(in_file, line, '\4');  // read "BFM" followed by space and control-D
            if (line.compare("BFM ") != 0) {
                break;
            }
            in_file.read(reinterpret_cast<char *>(&numRows), sizeof(uint32_t));  // read number of rows
            std::getline(in_file, line, '\4');                                   // read control-D
            in_file.read(reinterpret_cast<char *>(&numCols), sizeof(uint32_t));  // read number of columns
            num_bytes = numRows * numCols * sizeof(float);
            in_file.seekg(num_bytes, in_file.cur);                               // read data

            if (numArrays == numArrayToFindSize) {
                numMemoryBytes += num_bytes;
            }
            numArrays++;
        }
        in_file.close();
    } else {
        fprintf(stderr, "Failed to open %s for reading in GetKaldiArkInfo()!\n", fileName);
        exit(-1);
    }

    if (ptrNumArrays != NULL) *ptrNumArrays = numArrays;
    if (ptrNumMemoryBytes != NULL) *ptrNumMemoryBytes = numMemoryBytes;
}

void LoadKaldiArkArray(const char *fileName, uint32_t arrayIndex, std::string &ptrName, std::vector<uint8_t> &memory,
                       uint32_t *ptrNumRows, uint32_t *ptrNumColumns, uint32_t *ptrNumBytesPerElement) {
    std::ifstream in_file(fileName, std::ios::binary);
    if (in_file.good()) {
        uint32_t i = 0;
        while (i < arrayIndex) {
            std::string line;
            uint32_t numRows = 0u, numCols = 0u;
            std::getline(in_file, line, '\0');  // read variable length name followed by space and NUL
            std::getline(in_file, line, '\4');  // read "BFM" followed by space and control-D
            if (line.compare("BFM ") != 0) {
                break;
            }
            in_file.read(reinterpret_cast<char *>(&numRows), sizeof(uint32_t));     // read number of rows
            std::getline(in_file, line, '\4');                                     // read control-D
            in_file.read(reinterpret_cast<char *>(&numCols), sizeof(uint32_t));     // read number of columns
            in_file.seekg(numRows * numCols * sizeof(float), in_file.cur);         // read data
            i++;
        }
        if (!in_file.eof()) {
            std::string line;
            std::getline(in_file, ptrName, '\0');     // read variable length name followed by space and NUL
            std::getline(in_file, line, '\4');       // read "BFM" followed by space and control-D
            if (line.compare("BFM ") != 0) {
                fprintf(stderr, "Cannot find array specifier in file %s in LoadKaldiArkArray()!\n", fileName);
                exit(-1);
            }
            in_file.read(reinterpret_cast<char *>(ptrNumRows), sizeof(uint32_t));        // read number of rows
            std::getline(in_file, line, '\4');                                            // read control-D
            in_file.read(reinterpret_cast<char *>(ptrNumColumns), sizeof(uint32_t));    // read number of columns
            in_file.read(reinterpret_cast<char *>(&memory.front()),
                         *ptrNumRows * *ptrNumColumns * sizeof(float));  // read array data
        }
        in_file.close();
    } else {
        fprintf(stderr, "Failed to open %s for reading in GetKaldiArkInfo()!\n", fileName);
        exit(-1);
    }

    *ptrNumBytesPerElement = sizeof(float);
}

void SaveKaldiArkArray(const char *fileName,
                       bool shouldAppend,
                       std::string name,
                       void *ptrMemory,
                       uint32_t numRows,
                       uint32_t numColumns) {
    std::ios_base::openmode mode = std::ios::binary;
    if (shouldAppend) {
        mode |= std::ios::app;
    }
    std::ofstream out_file(fileName, mode);
    if (out_file.good()) {
        out_file.write(name.c_str(), name.length());  // write name
        out_file.write("\0", 1);
        out_file.write("BFM ", 4);
        out_file.write("\4", 1);
        out_file.write(reinterpret_cast<char *>(&numRows), sizeof(uint32_t));
        out_file.write("\4", 1);
        out_file.write(reinterpret_cast<char *>(&numColumns), sizeof(uint32_t));
        out_file.write(reinterpret_cast<char *>(ptrMemory), numRows * numColumns * sizeof(float));
        out_file.close();
    } else {
        throw std::runtime_error(std::string("Failed to open %s for writing in SaveKaldiArkArray()!\n") + fileName);
    }
}

float ScaleFactorForQuantization(void *ptrFloatMemory, float targetMax, uint32_t numElements) {
    float *ptrFloatFeat = reinterpret_cast<float *>(ptrFloatMemory);
    float max = 0.0;
    float scaleFactor;

    for (uint32_t i = 0; i < numElements; i++) {
        if (fabs(ptrFloatFeat[i]) > max) {
            max = fabs(ptrFloatFeat[i]);
        }
    }

    if (max == 0) {
        scaleFactor = 1.0;
    } else {
        scaleFactor = targetMax / max;
    }

    return (scaleFactor);
}

void ClearScoreError(score_error_t *error) {
    error->numScores = 0;
    error->numErrors = 0;
    error->maxError = 0.0;
    error->rmsError = 0.0;
    error->sumError = 0.0;
    error->sumRmsError = 0.0;
    error->sumSquaredError = 0.0;
    error->maxRelError = 0.0;
    error->sumRelError = 0.0;
    error->sumSquaredRelError = 0.0;
}

void UpdateScoreError(score_error_t *error, score_error_t *totalError) {
    totalError->numErrors += error->numErrors;
    totalError->numScores += error->numScores;
    totalError->sumRmsError += error->rmsError;
    totalError->sumError += error->sumError;
    totalError->sumSquaredError += error->sumSquaredError;
    if (error->maxError > totalError->maxError) {
        totalError->maxError = error->maxError;
    }
    totalError->sumRelError += error->sumRelError;
    totalError->sumSquaredRelError += error->sumSquaredRelError;
    if (error->maxRelError > totalError->maxRelError) {
        totalError->maxRelError = error->maxRelError;
    }
}

uint32_t CompareScores(float *ptrScoreArray,
                       void *ptrRefScoreArray,
                       score_error_t *scoreError,
                       uint32_t numRows,
                       uint32_t numColumns) {
    uint32_t numErrors = 0;

    ClearScoreError(scoreError);

    float *A = ptrScoreArray;
    float *B = reinterpret_cast<float *>(ptrRefScoreArray);
    for (uint32_t i = 0; i < numRows; i++) {
        for (uint32_t j = 0; j < numColumns; j++) {
            float score = A[i * numColumns + j];
            float refscore = B[i * numColumns + j];
            float error = fabs(refscore - score);
            float rel_error = error / (static_cast<float>(fabs(refscore)) + 1e-20f);
            float squared_error = error * error;
            float squared_rel_error = rel_error * rel_error;
            scoreError->numScores++;
            scoreError->sumError += error;
            scoreError->sumSquaredError += squared_error;
            if (error > scoreError->maxError) {
                scoreError->maxError = error;
            }
            scoreError->sumRelError += rel_error;
            scoreError->sumSquaredRelError += squared_rel_error;
            if (rel_error > scoreError->maxRelError) {
                scoreError->maxRelError = rel_error;
            }
            if (error > scoreError->threshold) {
                numErrors++;
            }
        }
    }
    scoreError->rmsError = sqrt(scoreError->sumSquaredError / (numRows * numColumns));
    scoreError->sumRmsError += scoreError->rmsError;
    scoreError->numErrors = numErrors;

    return (numErrors);
}

float StdDevError(score_error_t error) {
    return (sqrt(error.sumSquaredError / error.numScores
                 - (error.sumError / error.numScores) * (error.sumError / error.numScores)));
}

#if !defined(__arm__) && !defined(_M_ARM) && !defined(__aarch64__) && !defined(_M_ARM64)
#ifdef _WIN32
#include <intrin.h>
#include <windows.h>
#else

#include <cpuid.h>

#endif

inline void native_cpuid(unsigned int *eax, unsigned int *ebx,
                         unsigned int *ecx, unsigned int *edx) {
    size_t level = *eax;
#ifdef _WIN32
    int regs[4] = {static_cast<int>(*eax), static_cast<int>(*ebx), static_cast<int>(*ecx), static_cast<int>(*edx)};
    __cpuid(regs, level);
    *eax = static_cast<uint32_t>(regs[0]);
    *ebx = static_cast<uint32_t>(regs[1]);
    *ecx = static_cast<uint32_t>(regs[2]);
    *edx = static_cast<uint32_t>(regs[3]);
#else
    __get_cpuid(level, eax, ebx, ecx, edx);
#endif
}

// return GNA module frequency in MHz
float getGnaFrequencyMHz() {
    uint32_t eax = 1;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    uint32_t family = 0;
    uint32_t model = 0;
    const uint8_t sixth_family = 6;
    const uint8_t cannon_lake_model = 102;
    const uint8_t gemini_lake_model = 122;
    const uint8_t ice_lake_model = 126;
    const uint8_t next_model = 140;

    native_cpuid(&eax, &ebx, &ecx, &edx);
    family = (eax >> 8) & 0xF;

    // model is the concatenation of two fields
    // | extended model | model |
    // copy extended model data
    model = (eax >> 16) & 0xF;
    // shift
    model <<= 4;
    // copy model data
    model += (eax >> 4) & 0xF;

    if (family == sixth_family) {
        switch (model) {
            case cannon_lake_model:
            case ice_lake_model:
            case next_model:
                return 400;
            case gemini_lake_model:
                return 200;
            default:
                return 1;
        }
    } else {
        // counters not supported and we returns just default value
        return 1;
    }
}

#endif  // if not ARM

void printReferenceCompareResults(score_error_t const &totalError,
                                  size_t framesNum,
                                  std::ostream &stream) {
    stream << "         max error: " <<
           totalError.maxError << std::endl;
    stream << "         avg error: " <<
           totalError.sumError / totalError.numScores << std::endl;
    stream << "     avg rms error: " <<
           totalError.sumRmsError / framesNum << std::endl;
    stream << "       stdev error: " <<
           StdDevError(totalError) << std::endl << std::endl;
    stream << std::endl;
}

void printPerformanceCounters(std::map<std::string,
        InferenceEngine::InferenceEngineProfileInfo> const &utterancePerfMap,
                              size_t callsNum,
                              std::ostream &stream, std::string fullDeviceName) {
#if !defined(__arm__) && !defined(_M_ARM) && !defined(__aarch64__) && !defined(_M_ARM64)
    stream << std::endl << "Performance counts:" << std::endl;
    stream << std::setw(10) << std::right << "" << "Counter descriptions";
    stream << std::setw(22) << "Utt scoring time";
    stream << std::setw(18) << "Avg infer time";
    stream << std::endl;

    stream << std::setw(46) << "(ms)";
    stream << std::setw(24) << "(us per call)";
    stream << std::endl;

    for (const auto &it : utterancePerfMap) {
        std::string const &counter_name = it.first;
        float current_units = static_cast<float>(it.second.realTime_uSec);
        float call_units = current_units / callsNum;
        // if GNA HW counters
        // get frequency of GNA module
        float freq = getGnaFrequencyMHz();
        current_units /= freq * 1000;
        call_units /= freq;
        stream << std::setw(30) << std::left << counter_name.substr(4, counter_name.size() - 1);
        stream << std::setw(16) << std::right << current_units;
        stream << std::setw(21) << std::right << call_units;
        stream << std::endl;
    }
    stream << std::endl;
    std::cout << std::endl;
    std::cout << "Full device name: " << fullDeviceName << std::endl;
    std::cout << std::endl;
#endif
}

void getPerformanceCounters(InferenceEngine::InferRequest &request,
                            std::map<std::string, InferenceEngine::InferenceEngineProfileInfo> &perfCounters) {
    auto retPerfCounters = request.GetPerformanceCounts();

    for (const auto &pair : retPerfCounters) {
        perfCounters[pair.first] = pair.second;
    }
}

void sumPerformanceCounters(std::map<std::string, InferenceEngine::InferenceEngineProfileInfo> const &perfCounters,
                            std::map<std::string, InferenceEngine::InferenceEngineProfileInfo> &totalPerfCounters) {
    for (const auto &pair : perfCounters) {
        totalPerfCounters[pair.first].realTime_uSec += pair.second.realTime_uSec;
    }
}

std::vector<std::string> ParseScaleFactors(const std::string& str) {
    std::vector<std::string> scaleFactorInput;

    if (!str.empty()) {
        std::string outStr;
        std::istringstream stream(str);
        int i = 0;
        while (getline(stream, outStr, ',')) {
            auto floatScaleFactor  = std::stof(outStr);
            if (floatScaleFactor <= 0.0f) {
                throw std::logic_error("Scale factor for input #" + std::to_string(i)
                    + " (counting from zero) is out of range (must be positive).");
            }
            scaleFactorInput.push_back(outStr);
            i++;
        }
    } else {
        throw std::logic_error("Scale factor need to be specified via -sf option if you are using -q user");
    }
    return scaleFactorInput;
}

std::vector<std::string> ParseBlobName(std::string str) {
    std::vector<std::string> blobName;
    if (!str.empty()) {
        size_t pos_last = 0;
        size_t pos_next = 0;
        while ((pos_next = str.find(",", pos_last)) != std::string::npos) {
            blobName.push_back(str.substr(pos_last, pos_next - pos_last));
            pos_last = pos_next + 1;
        }
        blobName.push_back(str.substr(pos_last));
    }
    return blobName;
}

bool ParseAndCheckCommandLine(int argc, char *argv[]) {
    // ---------------------------Parsing and validation of input args--------------------------------------
    slog::info << "Parsing input parameters" << slog::endl;

    gflags::ParseCommandLineNonHelpFlags(&argc, &argv, true);
    if (FLAGS_h) {
        showUsage();
        showAvailableDevices();
        return false;
    }
    bool isDumpMode = !FLAGS_wg.empty() || !FLAGS_we.empty();

    // input not required only in dump mode and if external scale factor provided
    if (FLAGS_i.empty() && (!isDumpMode || FLAGS_q.compare("user") != 0)) {
        if (isDumpMode) {
            throw std::logic_error("In model dump mode either static quantization is used (-i) or user scale"
                                   " factor need to be provided. See -q user option");
        }
        throw std::logic_error("Input file not set. Please use -i.");
    }

    if (FLAGS_m.empty() && FLAGS_rg.empty()) {
        throw std::logic_error("Either IR file (-m) or GNAModel file (-rg) need to be set.");
    }

    if ((!FLAGS_m.empty() && !FLAGS_rg.empty())) {
        throw std::logic_error("Only one of -m and -rg is allowed.");
    }

    std::vector<std::string> supportedDevices = {
            "CPU",
            "GPU",
            "GNA_AUTO",
            "GNA_HW",
            "GNA_SW_EXACT",
            "GNA_SW",
            "GNA_SW_FP32",
            "HETERO:GNA,CPU",
            "HETERO:GNA_HW,CPU",
            "HETERO:GNA_SW_EXACT,CPU",
            "HETERO:GNA_SW,CPU",
            "HETERO:GNA_SW_FP32,CPU",
            "MYRIAD"
    };

    if (std::find(supportedDevices.begin(), supportedDevices.end(), FLAGS_d) == supportedDevices.end()) {
        throw std::logic_error("Specified device is not supported.");
    }

    uint32_t batchSize = (uint32_t) FLAGS_bs;
    if ((batchSize < 1) || (batchSize > 8)) {
        throw std::logic_error("Batch size out of range (1..8).");
    }

    /** default is a static quantization **/
    if ((FLAGS_q.compare("static") != 0) && (FLAGS_q.compare("dynamic") != 0) && (FLAGS_q.compare("user") != 0)) {
        throw std::logic_error("Quantization mode not supported (static, dynamic, user).");
    }

    if (FLAGS_q.compare("dynamic") == 0) {
        throw std::logic_error("Dynamic quantization not yet supported.");
    }

    if (FLAGS_qb != 16 && FLAGS_qb != 8) {
        throw std::logic_error("Only 8 or 16 bits supported.");
    }

    if (FLAGS_nthreads <= 0) {
        throw std::logic_error("Invalid value for 'nthreads' argument. It must be greater that or equal to 0");
    }

    if (FLAGS_cw_r < 0) {
        throw std::logic_error("Invalid value for 'cw_r' argument. It must be greater than or equal to 0");
    }

    if (FLAGS_cw_l < 0) {
        throw std::logic_error("Invalid value for 'cw_l' argument. It must be greater than or equal to 0");
    }

    if (FLAGS_pwl_me < 0.0 || FLAGS_pwl_me > 100.0) {
        throw std::logic_error("Invalid value for 'pwl_me' argument. It must be greater than 0.0 and less than 100.0");
    }

    return true;
}

/**
 * @brief The entry point for inference engine automatic speech recognition sample
 * @file speech_sample/main.cpp
 * @example speech_sample/main.cpp
 */
int main(int argc, char *argv[]) {
    try {
        slog::info << "InferenceEngine: " << GetInferenceEngineVersion() << slog::endl;

        // ------------------------------ Parsing and validation of input args ---------------------------------
        if (!ParseAndCheckCommandLine(argc, argv)) {
            return 0;
        }

        if (FLAGS_l.empty()) {
            slog::info << "No extensions provided" << slog::endl;
        }

        auto isFeature = [&](const std::string xFeature) { return FLAGS_d.find(xFeature) != std::string::npos; };

        bool useGna = isFeature("GNA");
        bool useHetero = isFeature("HETERO");
        std::string deviceStr =
                useHetero && useGna ? "HETERO:GNA,CPU" : FLAGS_d.substr(0, (FLAGS_d.find("_")));
        uint32_t batchSize = (FLAGS_cw_r > 0 || FLAGS_cw_l > 0) ? 1 : (uint32_t) FLAGS_bs;

        std::vector<std::string> inputArkFiles;
        std::vector<uint32_t> numBytesThisUtterance;
        uint32_t numUtterances(0);
        if (!FLAGS_i.empty()) {
            std::string outStr;
            std::istringstream stream(FLAGS_i);

            uint32_t currentNumUtterances(0), currentNumBytesThisUtterance(0);
            while (getline(stream, outStr, ',')) {
                std::string filename(fileNameNoExt(outStr) + ".ark");
                inputArkFiles.push_back(filename);

                GetKaldiArkInfo(filename.c_str(), 0, &currentNumUtterances, &currentNumBytesThisUtterance);
                if (numUtterances == 0) {
                    numUtterances = currentNumUtterances;
                } else if (currentNumUtterances != numUtterances) {
                    throw std::logic_error("Incorrect input files. Number of utterance must be the same for all ark files");
                }
                numBytesThisUtterance.push_back(currentNumBytesThisUtterance);
            }
        }
        size_t numInputArkFiles(inputArkFiles.size());
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 1. Load inference engine -------------------------------------
        slog::info << "Loading Inference Engine" << slog::endl;
        Core ie;
        CNNNetwork network;
        ExecutableNetwork executableNet;

        /** Printing device version **/
        slog::info << "Device info: " << slog::endl;
        std::cout << ie.GetVersions(deviceStr) << std::endl;
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 2. Read a model in OpenVINO Intermediate Representation (.xml and .bin files) or ONNX (.onnx file) format
        slog::info << "Loading network files" << slog::endl;

        if (!FLAGS_m.empty()) {
            /** Read network model **/
            network = ie.ReadNetwork(FLAGS_m);
            CheckNumberOfInputs(network.getInputsInfo().size(), numInputArkFiles);
            // -------------------------------------------------------------------------------------------------

            // --------------------------- Set batch size ---------------------------------------------------
            /** Set batch size.  Unlike in imaging, batching in time (rather than space) is done for speech recognition. **/
            network.setBatchSize(batchSize);
            slog::info << "Batch size is " << std::to_string(network.getBatchSize())
                       << slog::endl;
        }

        // -----------------------------------------------------------------------------------------------------

        // --------------------------- Set parameters and scale factors -------------------------------------
        /** Setting parameter for per layer metrics **/
        std::map<std::string, std::string> gnaPluginConfig;
        std::map<std::string, std::string> genericPluginConfig;
        if (useGna) {
            std::string gnaDevice =
                    useHetero ? FLAGS_d.substr(FLAGS_d.find("GNA"), FLAGS_d.find(",") - FLAGS_d.find("GNA")) : FLAGS_d;
            gnaPluginConfig[GNAConfigParams::KEY_GNA_DEVICE_MODE] =
                    gnaDevice.find("_") == std::string::npos ? "GNA_AUTO" : gnaDevice;
        }

        if (FLAGS_pc) {
            genericPluginConfig[PluginConfigParams::KEY_PERF_COUNT] = PluginConfigParams::YES;
        }

        if (FLAGS_q.compare("user") == 0) {
            if (!FLAGS_rg.empty()) {
                slog::warn << "Custom scale factor will be ignored - using scale factor from provided imported gna model: "
                           << FLAGS_rg << slog::endl;
            } else {
                auto scaleFactorInput = ParseScaleFactors(FLAGS_sf);
                if (numInputArkFiles != scaleFactorInput.size()) {
                    std::string errMessage("Incorrect command line for multiple inputs: "
                        + std::to_string(scaleFactorInput.size()) + " scale factors provided for "
                        + std::to_string(numInputArkFiles) + " input files.");
                    throw std::logic_error(errMessage);
                }

                for (size_t i = 0; i < scaleFactorInput.size(); ++i) {
                    slog::info << "For input " << i << " using scale factor of " << scaleFactorInput[i] << slog::endl;
                    std::string scaleFactorConfigKey = GNA_CONFIG_KEY(SCALE_FACTOR) + std::string("_") + std::to_string(i);
                    gnaPluginConfig[scaleFactorConfigKey] = scaleFactorInput[i];
                }
            }
        } else {
            // "static" quantization with calculated scale factor
            if (!FLAGS_rg.empty()) {
                slog::info << "Using scale factor from provided imported gna model: " << FLAGS_rg << slog::endl;
            } else {
                for (size_t i = 0; i < numInputArkFiles; i++) {
                    auto inputArkName = inputArkFiles[i].c_str();
                    std::string name;
                    std::vector<uint8_t> ptrFeatures;
                    uint32_t numArrays(0), numBytes(0), numFrames(0), numFrameElements(0), numBytesPerElement(0);
                    GetKaldiArkInfo(inputArkName, 0, &numArrays, &numBytes);
                    ptrFeatures.resize(numBytes);
                    LoadKaldiArkArray(inputArkName,
                        0,
                        name,
                        ptrFeatures,
                        &numFrames,
                        &numFrameElements,
                        &numBytesPerElement);
                    auto floatScaleFactor =
                        ScaleFactorForQuantization(ptrFeatures.data(), MAX_VAL_2B_FEAT, numFrames * numFrameElements);
                    slog::info << "Using scale factor of " << floatScaleFactor << " calculated from first utterance."
                        << slog::endl;
                    std::string scaleFactorConfigKey = GNA_CONFIG_KEY(SCALE_FACTOR) + std::string("_") + std::to_string(i);
                    gnaPluginConfig[scaleFactorConfigKey] = std::to_string(floatScaleFactor);
                }
            }
        }

        if (FLAGS_qb == 8) {
            gnaPluginConfig[GNAConfigParams::KEY_GNA_PRECISION] = "I8";
        } else {
            gnaPluginConfig[GNAConfigParams::KEY_GNA_PRECISION] = "I16";
        }

        gnaPluginConfig[GNAConfigParams::KEY_GNA_LIB_N_THREADS] = std::to_string((FLAGS_cw_r > 0 || FLAGS_cw_l > 0) ? 1 : FLAGS_nthreads);
        gnaPluginConfig[GNA_CONFIG_KEY(COMPACT_MODE)] = CONFIG_VALUE(NO);
        gnaPluginConfig[GNA_CONFIG_KEY(PWL_MAX_ERROR_PERCENT)] = std::to_string(FLAGS_pwl_me);
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- Write model to file --------------------------------------------------
        // Embedded GNA model dumping (for Intel(R) Speech Enabling Developer Kit)
        if (!FLAGS_we.empty()) {
            gnaPluginConfig[GNAConfigParams::KEY_GNA_FIRMWARE_MODEL_IMAGE] = FLAGS_we;
            gnaPluginConfig[GNAConfigParams::KEY_GNA_FIRMWARE_MODEL_IMAGE_GENERATION] = FLAGS_we_gen;
        }
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 3. Loading model to the device ------------------------------------------

        if (useGna) {
            genericPluginConfig.insert(std::begin(gnaPluginConfig), std::end(gnaPluginConfig));
        }
        auto t0 = Time::now();
        std::vector<std::string> outputs;

        if (!FLAGS_oname.empty()) {
            std::vector<std::string> output_names = ParseBlobName(FLAGS_oname);
            std::vector<size_t> ports;
            for (const auto& outBlobName : output_names) {
                int pos_layer = outBlobName.rfind(":");
                if (pos_layer == -1) {
                    throw std::logic_error(std::string("Output ") + std::string(outBlobName)
                    + std::string(" doesn't have a port"));
                }
                outputs.push_back(outBlobName.substr(0, pos_layer));
                try {
                    ports.push_back(std::stoi(outBlobName.substr(pos_layer + 1)));
                } catch (const std::exception &) {
                    throw std::logic_error("Ports should have integer type");
                }
            }

            for (size_t i = 0; i < outputs.size(); i++) {
                network.addOutput(outputs[i], ports[i]);
            }
        }
        if (!FLAGS_m.empty()) {
            slog::info << "Loading model to the device" << slog::endl;
            executableNet = ie.LoadNetwork(network, deviceStr, genericPluginConfig);
        } else {
            slog::info << "Importing model to the device" << slog::endl;
            executableNet = ie.ImportNetwork(FLAGS_rg.c_str(), deviceStr, genericPluginConfig);
        }
        ms loadTime = std::chrono::duration_cast<ms>(Time::now() - t0);
        slog::info << "Model loading time " << loadTime.count() << " ms" << slog::endl;

        // --------------------------- Exporting gna model using InferenceEngine AOT API---------------------
        if (!FLAGS_wg.empty()) {
            slog::info << "Writing GNA Model to file " << FLAGS_wg << slog::endl;
            t0 = Time::now();
            executableNet.Export(FLAGS_wg);
            ms exportTime = std::chrono::duration_cast<ms>(Time::now() - t0);
            slog::info << "Exporting time " << exportTime.count() << " ms" << slog::endl;
            return 0;
        }

        if (!FLAGS_we.empty()) {
            slog::info << "Exported GNA embedded model to file " << FLAGS_we << slog::endl;
            if (!FLAGS_we_gen.empty()) {
                slog::info << "GNA embedded model export done for GNA generation: " << FLAGS_we_gen << slog::endl;
            }
            return 0;
        }


        // --------------------------- 4. Create infer request --------------------------------------------------
        std::vector<InferRequestStruct> inferRequests((FLAGS_cw_r > 0 || FLAGS_cw_l > 0) ? 1 : FLAGS_nthreads);
        for (auto& inferRequest : inferRequests) {
            inferRequest = {executableNet.CreateInferRequest(), -1, batchSize};
        }
        // ---------------------------------------------------------------------------------------------------------

        // --------------------------- 5. Configure input & output --------------------------------------------------

        //--- Prepare input blobs ----------------------------------------------
        /** Taking information about all topology inputs **/
        ConstInputsDataMap cInputInfo = executableNet.GetInputsInfo();
        CheckNumberOfInputs(cInputInfo.size(), numInputArkFiles);

        /** Stores all input blobs data **/
        std::vector<Blob::Ptr> ptrInputBlobs;
        if (!FLAGS_iname.empty()) {
            std::vector<std::string> inputNameBlobs = ParseBlobName(FLAGS_iname);
            if (inputNameBlobs.size() != cInputInfo.size()) {
                std::string errMessage(std::string("Number of network inputs ( ") + std::to_string(cInputInfo.size()) +
                                       " ) is not equal to the number of inputs entered in the -iname argument ( " +
                                       std::to_string(inputNameBlobs.size()) + " ).");
                throw std::logic_error(errMessage);
            }
            for (const auto& input : inputNameBlobs) {
                Blob::Ptr blob = inferRequests.begin()->inferRequest.GetBlob(input);
                if (!blob) {
                    std::string errMessage("No blob with name : " + input);
                    throw std::logic_error(errMessage);
                }
                ptrInputBlobs.push_back(blob);
            }
        } else {
            for (const auto& input : cInputInfo) {
                ptrInputBlobs.push_back(inferRequests.begin()->inferRequest.GetBlob(input.first));
            }
        }
        InputsDataMap inputInfo;
        if (!FLAGS_m.empty()) {
            inputInfo = network.getInputsInfo();
        }
        /** Configure input precision if model is loaded from IR **/
        for (auto &item : inputInfo) {
            Precision inputPrecision = Precision::FP32;  // specify Precision::I16 to provide quantized inputs
            item.second->setPrecision(inputPrecision);
        }

        // ---------------------------------------------------------------------

        //--- Prepare output blobs ---------------------------------------------
        ConstOutputsDataMap cOutputInfo(executableNet.GetOutputsInfo());
        OutputsDataMap outputInfo;
        if (!FLAGS_m.empty()) {
            outputInfo = network.getOutputsInfo();
        }
        std::vector<Blob::Ptr> ptrOutputBlob;
        if (!outputs.empty()) {
            for (const auto& output : outputs) {
                Blob::Ptr blob = inferRequests.begin()->inferRequest.GetBlob(output);
                if (!blob) {
                    std::string errMessage("No blob with name : " + output);
                    throw std::logic_error(errMessage);
                }
                ptrOutputBlob.push_back(blob);
            }
        } else {
            for (auto& output : cOutputInfo) {
                ptrOutputBlob.push_back(inferRequests.begin()->inferRequest.GetBlob(output.first));
            }
        }

        for (auto &item : outputInfo) {
            DataPtr outData = item.second;
            if (!outData) {
                throw std::logic_error("output data pointer is not valid");
            }

            Precision outputPrecision = Precision::FP32;  // specify Precision::I32 to retrieve quantized outputs
            outData->setPrecision(outputPrecision);
        }
        // ---------------------------------------------------------------------
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 6. Do inference --------------------------------------------------------
        std::vector<std::string> output_name_files;
        std::vector<std::string> reference_name_files;
        size_t count_file = 1;
        if (!FLAGS_o.empty()) {
            output_name_files = ParseBlobName(FLAGS_o);
            if (output_name_files.size() != outputs.size() && !outputs.empty()) {
                throw std::logic_error("The number of output files is not equal to the number of network outputs.");
            }
            count_file = output_name_files.empty() ? 1 : output_name_files.size();
        }
        if (!FLAGS_r.empty()) {
            reference_name_files = ParseBlobName(FLAGS_r);
            if (reference_name_files.size() != outputs.size() && !outputs.empty()) {
                throw std::logic_error("The number of reference files is not equal to the number of network outputs.");
            }
            count_file = reference_name_files.empty() ? 1 : reference_name_files.size();
        }
        for (size_t next_output = 0; next_output < count_file; next_output++) {
            std::vector<std::vector<uint8_t>> ptrUtterances;
            std::vector<uint8_t> ptrScores;
            std::vector<uint8_t> ptrReferenceScores;
            score_error_t frameError, totalError;

            ptrUtterances.resize(inputArkFiles.size());

            // initialize memory state before starting
            for (auto &&state : inferRequests.begin()->inferRequest.QueryState()) {
                state.Reset();
            }

            /** Work with each utterance **/
            for (uint32_t utteranceIndex = 0; utteranceIndex < numUtterances; ++utteranceIndex) {
                std::map<std::string, InferenceEngine::InferenceEngineProfileInfo> utterancePerfMap;
                std::string uttName;
                uint32_t numFrames(0), n(0);
                std::vector<uint32_t> numFrameElementsInput;

                uint32_t numFramesReference(0), numFrameElementsReference(0), numBytesPerElementReference(0),
                        numBytesReferenceScoreThisUtterance(0);
                auto dims = outputs.empty() ? cOutputInfo.rbegin()->second->getDims() : cOutputInfo[outputs[next_output]]->getDims();
                const auto numScoresPerFrame = std::accumulate(std::begin(dims), std::end(dims), size_t{1}, std::multiplies<size_t>());

                slog::info << "Number scores per frame : " << numScoresPerFrame << slog::endl;

                /** Get information from ark file for current utterance **/
                numFrameElementsInput.resize(numInputArkFiles);
                for (size_t i = 0; i < inputArkFiles.size(); i++) {
                    std::vector<uint8_t> ptrUtterance;
                    auto inputArkFilename = inputArkFiles[i].c_str();
                    uint32_t currentNumFrames(0), currentNumFrameElementsInput(0), currentNumBytesPerElementInput(0);
                    GetKaldiArkInfo(inputArkFilename, utteranceIndex, &n, &numBytesThisUtterance[i]);
                    ptrUtterance.resize(numBytesThisUtterance[i]);
                    LoadKaldiArkArray(inputArkFilename,
                                      utteranceIndex,
                                      uttName,
                                      ptrUtterance,
                                      &currentNumFrames,
                                      &currentNumFrameElementsInput,
                                      &currentNumBytesPerElementInput);
                    if (numFrames == 0) {
                        numFrames = currentNumFrames;
                    } else if (numFrames != currentNumFrames) {
                        std::string errMessage(
                                "Number of frames in ark files is different: " + std::to_string(numFrames) +
                                " and " + std::to_string(currentNumFrames));
                        throw std::logic_error(errMessage);
                    }

                    ptrUtterances[i] = ptrUtterance;
                    numFrameElementsInput[i] = currentNumFrameElementsInput;
                }

                int i = 0;
                for (auto &ptrInputBlob : ptrInputBlobs) {
                    if (ptrInputBlob->size() != numFrameElementsInput[i++] * batchSize) {
                        throw std::logic_error("network input size(" + std::to_string(ptrInputBlob->size()) +
                                               ") mismatch to ark file size (" +
                                               std::to_string(numFrameElementsInput[i - 1] * batchSize) + ")");
                    }
                }

                ptrScores.resize(numFrames * numScoresPerFrame * sizeof(float));
                if (!FLAGS_r.empty()) {
                    /** Read ark file with reference scores **/
                    std::string refUtteranceName;
                    GetKaldiArkInfo(reference_name_files[next_output].c_str(), utteranceIndex, &n, &numBytesReferenceScoreThisUtterance);
                    ptrReferenceScores.resize(numBytesReferenceScoreThisUtterance);
                    LoadKaldiArkArray(reference_name_files[next_output].c_str(),
                                      utteranceIndex,
                                      refUtteranceName,
                                      ptrReferenceScores,
                                      &numFramesReference,
                                      &numFrameElementsReference,
                                      &numBytesPerElementReference);
                }

                double totalTime = 0.0;

                std::cout << "Utterance " << utteranceIndex << ": " << std::endl;

                ClearScoreError(&totalError);
                totalError.threshold = frameError.threshold = MAX_SCORE_DIFFERENCE;
                auto outputFrame = &ptrScores.front();
                std::vector<uint8_t *> inputFrame;
                for (auto &ut : ptrUtterances) {
                    inputFrame.push_back(&ut.front());
                }

                std::map<std::string, InferenceEngine::InferenceEngineProfileInfo> callPerfMap;

                size_t frameIndex = 0;
                uint32_t numFramesArkFile = numFrames;
                numFrames += FLAGS_cw_l + FLAGS_cw_r;
                uint32_t numFramesThisBatch{batchSize};

                auto t0 = Time::now();
                auto t1 = t0;

                while (frameIndex <= numFrames) {
                    if (frameIndex == numFrames) {
                        if (std::find_if(inferRequests.begin(),
                                         inferRequests.end(),
                                         [&](InferRequestStruct x) { return (x.frameIndex != -1); }) ==
                            inferRequests.end()) {
                            break;
                        }
                    }

                    bool inferRequestFetched = false;
                    /** Start inference loop **/
                    for (auto &inferRequest : inferRequests) {
                        if (frameIndex == numFrames) {
                            numFramesThisBatch = 1;
                        } else {
                            numFramesThisBatch = (numFrames - frameIndex < batchSize) ? (numFrames - frameIndex)
                                                                                      : batchSize;
                        }

                        if (inferRequest.frameIndex != -1) {
                            StatusCode code = inferRequest.inferRequest.Wait(
                                    InferenceEngine::IInferRequest::WaitMode::RESULT_READY);

                            if (code != StatusCode::OK) {
                                if (!useHetero) continue;
                                if (code != StatusCode::INFER_NOT_STARTED) continue;
                            }
                            ConstOutputsDataMap newOutputInfo;
                            if (inferRequest.frameIndex >= 0) {
                                if (!FLAGS_o.empty()) {
                                    /* Prepare output data for save to file in future */
                                    outputFrame =
                                            &ptrScores.front() +
                                            numScoresPerFrame * sizeof(float) * (inferRequest.frameIndex);
                                    if (!outputs.empty()) {
                                        newOutputInfo[outputs[next_output]] = cOutputInfo[outputs[next_output]];
                                    } else {
                                        newOutputInfo = cOutputInfo;
                                    }
                                    Blob::Ptr outputBlob = inferRequest.inferRequest.GetBlob(newOutputInfo.rbegin()->first);
                                    MemoryBlob::CPtr moutput = as<MemoryBlob>(outputBlob);

                                    if (!moutput) {
                                        throw std::logic_error("We expect output to be inherited from MemoryBlob, "
                                                               "but in fact we were not able to cast output to MemoryBlob");
                                    }
                                    // locked memory holder should be alive all time while access to its buffer happens
                                    auto moutputHolder = moutput->rmap();
                                    auto byteSize =
                                            numScoresPerFrame * sizeof(float);
                                    std::memcpy(outputFrame,
                                                moutputHolder.as<const void *>(),
                                                byteSize);
                                }
                                if (!FLAGS_r.empty()) {
                                    /** Compare output data with reference scores **/
                                    if (!outputs.empty()) {
                                        newOutputInfo[outputs[next_output]] = cOutputInfo[outputs[next_output]];
                                    } else {
                                        newOutputInfo = cOutputInfo;
                                    }
                                    Blob::Ptr outputBlob = inferRequest.inferRequest.GetBlob(newOutputInfo.rbegin()->first);
                                    MemoryBlob::CPtr moutput = as<MemoryBlob>(outputBlob);
                                    if (!moutput) {
                                        throw std::logic_error("We expect output to be inherited from MemoryBlob, "
                                                               "but in fact we were not able to cast output to MemoryBlob");
                                    }
                                    // locked memory holder should be alive all time while access to its buffer happens
                                    auto moutputHolder = moutput->rmap();
                                    CompareScores(moutputHolder.as<float *>(),
                                                  &ptrReferenceScores[inferRequest.frameIndex *
                                                                      numFrameElementsReference *
                                                                      numBytesPerElementReference],
                                                  &frameError,
                                                  inferRequest.numFramesThisBatch,
                                                  numFrameElementsReference);
                                    UpdateScoreError(&frameError, &totalError);
                                }
                                if (FLAGS_pc) {
                                    // retrieve new counters
                                    getPerformanceCounters(inferRequest.inferRequest, callPerfMap);
                                    // summarize retrieved counters with all previous
                                    sumPerformanceCounters(callPerfMap, utterancePerfMap);
                                }
                            }
                        }

                        if (frameIndex == numFrames) {
                            inferRequest.frameIndex = -1;
                            continue;
                        }

                        /** Prepare input blobs**/
                        ptrInputBlobs.clear();
                        if (FLAGS_iname.empty()) {
                            for (auto &input : cInputInfo) {
                                ptrInputBlobs.push_back(inferRequest.inferRequest.GetBlob(input.first));
                            }
                        } else {
                            std::vector<std::string> inputNameBlobs = ParseBlobName(FLAGS_iname);
                            for (const auto& input : inputNameBlobs) {
                                Blob::Ptr blob = inferRequests.begin()->inferRequest.GetBlob(input);
                                if (!blob) {
                                    std::string errMessage("No blob with name : " + input);
                                    throw std::logic_error(errMessage);
                                }
                                ptrInputBlobs.push_back(blob);
                            }
                        }

                        for (size_t i = 0; i < numInputArkFiles; ++i) {
                            MemoryBlob::Ptr minput = as<MemoryBlob>(ptrInputBlobs[i]);
                            if (!minput) {
                                std::string errMessage("We expect ptrInputBlobs[" + std::to_string(i) +
                                          "] to be inherited from MemoryBlob, " +
                                          "but in fact we were not able to cast input blob to MemoryBlob");
                                throw std::logic_error(errMessage);
                            }
                            // locked memory holder should be alive all time while access to its buffer happens
                            auto minputHolder = minput->wmap();

                            std::memcpy(minputHolder.as<void *>(),
                                        inputFrame[i],
                                        minput->byteSize());
                        }

                        int index = static_cast<int>(frameIndex) - (FLAGS_cw_l + FLAGS_cw_r);
                        /** Start inference **/
                        inferRequest.inferRequest.StartAsync();
                        inferRequest.frameIndex = index < 0 ? -2 : index;
                        inferRequest.numFramesThisBatch = numFramesThisBatch;

                        frameIndex += numFramesThisBatch;
                        for (size_t j = 0; j < inputArkFiles.size(); j++) {
                            if (FLAGS_cw_l > 0 || FLAGS_cw_r > 0) {
                                int idx = frameIndex - FLAGS_cw_l;
                                if (idx > 0 && idx < static_cast<int>(numFramesArkFile)) {
                                    inputFrame[j] += sizeof(float) * numFrameElementsInput[j] * numFramesThisBatch;
                                } else if (idx >= static_cast<int>(numFramesArkFile)) {
                                    inputFrame[j] = &ptrUtterances[j].front() +
                                                    (numFramesArkFile - 1) * sizeof(float) * numFrameElementsInput[j] *
                                                    numFramesThisBatch;
                                } else if (idx <= 0) {
                                    inputFrame[j] = &ptrUtterances[j].front();
                                }
                            } else {
                                inputFrame[j] += sizeof(float) * numFrameElementsInput[j] * numFramesThisBatch;
                            }
                        }
                        inferRequestFetched |= true;
                    }
                    /** Inference was finished for current frame **/
                    if (!inferRequestFetched) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
                }
                t1 = Time::now();

                fsec fs = t1 - t0;
                ms d = std::chrono::duration_cast<ms>(fs);
                totalTime += d.count();

                // resetting state between utterances
                for (auto &&state : inferRequests.begin()->inferRequest.QueryState()) {
                    state.Reset();
                }

                if (!FLAGS_o.empty()) {
                    /* Save output data to file */
                    bool shouldAppend = (utteranceIndex == 0) ? false : true;
                    SaveKaldiArkArray(output_name_files[next_output].c_str(), shouldAppend, uttName, &ptrScores.front(),
                                      numFramesArkFile, numScoresPerFrame);
                }

                /** Show performance results **/
                std::cout << "Total time in Infer (HW and SW):\t" << totalTime << " ms"
                          << std::endl;
                std::cout << "Frames in utterance:\t\t\t" << numFrames << " frames"
                          << std::endl;
                std::cout << "Average Infer time per frame:\t\t" << totalTime / static_cast<double>(numFrames) << " ms"
                          << std::endl;
                if (FLAGS_pc) {
                    // print
                    printPerformanceCounters(utterancePerfMap, frameIndex, std::cout, getFullDeviceName(ie, FLAGS_d));
                }
                if (!FLAGS_r.empty()) {
                    printReferenceCompareResults(totalError, numFrames, std::cout);
                }
                std::cout << "End of Utterance " << utteranceIndex << std::endl << std::endl;
            }
        }
        // -----------------------------------------------------------------------------------------------------
    }
    catch (const std::exception &error) {
        slog::err << error.what() << slog::endl;
        return 1;
    }
    catch (...) {
        slog::err << "Unknown/internal exception happened" << slog::endl;
        return 1;
    }

    slog::info << "Execution successful" << slog::endl;
    return 0;
}
