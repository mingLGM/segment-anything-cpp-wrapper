#include "sam.h"

#include <onnxruntime_cxx_api.h>

#include <codecvt>
#include <fstream>
#include <iostream>
#include <locale>
#include <mutex>
#include <opencv2/opencv.hpp>
#include <vector>

struct SamModel {
  Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "test"};
  Ort::SessionOptions sessionOptions[2];
  std::unique_ptr<Ort::Session> sessionPre, sessionSam;
  std::vector<int64_t> inputShapePre, outputShapePre, intermShapePre;
  Ort::MemoryInfo memoryInfo{Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)};
  bool bModelLoaded = false, bSamHQ = false, bEdgeSam = false;
  std::vector<float> outputTensorValuesPre, intermTensorValuesPre;
  mutable std::recursive_mutex recursive_mutex;

  char *inputNamesSam[6]{"image_embeddings", "point_coords",   "point_labels",
                         "mask_input",       "has_mask_input", "orig_im_size"},
      *inputNamesSamHQ[7]{"image_embeddings", "interm_embeddings", "point_coords", "point_labels",
                          "mask_input",       "has_mask_input",    "orig_im_size"},
      *inputNamesEdgeSam[3]{"image_embeddings", "point_coords", "point_labels"},
      *outputNamesSam[3]{"masks", "iou_predictions", "low_res_masks"},
      *outputNamesEdgeSam[2]{"scores", "masks"};

  SamModel(const Sam::Parameter& param) {
    for (auto& p : param.models) {
      std::ifstream f(p);
      if (!f.good()) {
        std::cerr << "Model file " << p << " not found" << std::endl;
        return;
      }
    }

    for (int i = 0; i < 2; i++) {
      auto& provider = param.providers[i];
      auto& option = sessionOptions[i];

      option.SetIntraOpNumThreads(param.threadsNumber);
      option.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

      if (provider.deviceType == 1) {
        OrtCUDAProviderOptions options;
        options.device_id = provider.gpuDeviceId;
        if (provider.gpuMemoryLimit > 0) {
          options.gpu_mem_limit = provider.gpuMemoryLimit;
        }
        option.AppendExecutionProvider_CUDA(options);
      }
    }

#if _MSC_VER
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    auto wpreModelPath = converter.from_bytes(param.models[0]);
    auto wsamModelPath = converter.from_bytes(param.models[1]);
#else
    auto wpreModelPath = param.models[0];
    auto wsamModelPath = param.models[1];
#endif

    sessionPre = std::make_unique<Ort::Session>(env, wpreModelPath.c_str(), sessionOptions[0]);
    int targetNumber[]{1, 6};

    bSamHQ = sessionPre->GetOutputCount() == 2;
    if (bSamHQ) {
      for (auto& v : targetNumber) {
        v++;
      }
    }

    if (sessionPre->GetInputCount() != 1 || sessionPre->GetOutputCount() != targetNumber[0]) {
      std::cerr << "Preprocessing model not loaded (invalid input/output count)" << std::endl;
      return;
    }

    sessionSam = std::make_unique<Ort::Session>(env, wsamModelPath.c_str(), sessionOptions[1]);
    const auto samOutputCount = sessionSam->GetOutputCount();

    bEdgeSam = samOutputCount == 2;
    if (bEdgeSam) {
      targetNumber[1] = 3;
    }

    if (sessionSam->GetInputCount() != targetNumber[1] || (samOutputCount != 3 && !bEdgeSam)) {
      std::cerr << "Model not loaded (invalid input/output count)" << std::endl;
      return;
    }

    inputShapePre = sessionPre->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    outputShapePre = sessionPre->GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    if (inputShapePre.size() != 4 || outputShapePre.size() != 4) {
      std::cerr << "Preprocessing model not loaded (invalid shape)" << std::endl;
      return;
    }

    if (bSamHQ) {
      intermShapePre = sessionSam->GetInputTypeInfo(1).GetTensorTypeAndShapeInfo().GetShape();
      if (intermShapePre.size() != 5) {
        std::cerr << "Model not loaded (invalid interm shape)" << std::endl;
        return;
      }
    }

    bModelLoaded = true;
  }

  cv::Size getInputSize() const {
    if (!bModelLoaded) return cv::Size(0, 0);
    return cv::Size(inputShapePre[3], inputShapePre[2]);
  }
  bool loadImage(const cv::Mat& image) {
    if (image.size() != cv::Size(inputShapePre[3], inputShapePre[2])) {
      std::cerr << "Image size not match" << std::endl;
      return false;
    }
    if (image.channels() != 3) {
      std::cerr << "Input is not a 3-channel image" << std::endl;
      return false;
    }

    std::vector<uint8_t> inputTensorValuesInt;
    std::vector<float> inputTensorValuesFloat;

#define SetInput(inputTensorValues)                                                           \
  inputTensorValues.resize(inputShapePre[0] * inputShapePre[1] * inputShapePre[2] *           \
                           inputShapePre[3]);                                                 \
  for (int i = 0; i < inputShapePre[2]; i++) {                                                \
    for (int j = 0; j < inputShapePre[3]; j++) {                                              \
      inputTensorValues[i * inputShapePre[3] + j] = image.at<cv::Vec3b>(i, j)[2];             \
      inputTensorValues[inputShapePre[2] * inputShapePre[3] + i * inputShapePre[3] + j] =     \
          image.at<cv::Vec3b>(i, j)[1];                                                       \
      inputTensorValues[2 * inputShapePre[2] * inputShapePre[3] + i * inputShapePre[3] + j] = \
          image.at<cv::Vec3b>(i, j)[0];                                                       \
    }                                                                                         \
  }                                                                                           \
  if (bEdgeSam) {                                                                             \
    for (auto& v : inputTensorValues) {                                                       \
      v /= 255.;                                                                              \
    }                                                                                         \
  }

    if (!bEdgeSam) {
      SetInput(inputTensorValuesInt);
    } else {
      SetInput(inputTensorValuesFloat);
    }

#define InputTensor(inputTensorValues, type)                                                     \
  Ort::Value::CreateTensor<type>(memoryInfo, inputTensorValues.data(), inputTensorValues.size(), \
                                 inputShapePre.data(), inputShapePre.size())

    auto inputTensor = bEdgeSam ? InputTensor(inputTensorValuesFloat, float)
                                : InputTensor(inputTensorValuesInt, uint8_t);

    std::vector<Ort::Value> outputTensors;

    outputTensorValuesPre = std::vector<float>(outputShapePre[0] * outputShapePre[1] *
                                               outputShapePre[2] * outputShapePre[3]);
    outputTensors.emplace_back(Ort::Value::CreateTensor<float>(
        memoryInfo, outputTensorValuesPre.data(), outputTensorValuesPre.size(),
        outputShapePre.data(), outputShapePre.size()));

    if (bSamHQ) {
      intermTensorValuesPre =
          std::vector<float>(intermShapePre[0] * intermShapePre[1] * intermShapePre[2] *
                             intermShapePre[3] * intermShapePre[4]);
      outputTensors.emplace_back(Ort::Value::CreateTensor<float>(
          memoryInfo, intermTensorValuesPre.data(), intermTensorValuesPre.size(),
          intermShapePre.data(), intermShapePre.size()));
    }

    Ort::RunOptions run_options;
    const char *inputNamesPre[] = {"input"}, *outputNamesPre[] = {"output", "interm_embeddings"};
    const char *inputNamesPreEdge[] = {"image"}, *outputNamesPreEdge[] = {"image_embeddings"};
    const auto inputNamesPre1 = bEdgeSam ? inputNamesPreEdge : inputNamesPre,
               outputNamesPre1 = bEdgeSam ? outputNamesPreEdge : outputNamesPre;
    sessionPre->Run(run_options, inputNamesPre1, &inputTensor, 1, outputNamesPre1,
                    outputTensors.data(), outputTensors.size());

    return true;
  }

  void getMask(const std::list<cv::Point>& points, const std::list<cv::Point>& negativePoints,
               const cv::Rect& roi, cv::Mat& outputMaskSam, double& iouValue) const {
    std::lock_guard<std::recursive_mutex> lock(recursive_mutex);
    const size_t maskInputSize = 256 * 256;
    float maskInputValues[maskInputSize] = {0}, hasMaskValues[] = {0},
          orig_im_size_values[] = {static_cast<float>(inputShapePre[2]),
                                   static_cast<float>(inputShapePre[3])};
    memset(maskInputValues, 0, sizeof(maskInputValues));

    const float imgWidth = static_cast<float>(inputShapePre[3]);
    const float imgHeight = static_cast<float>(inputShapePre[2]);
    std::vector<float> inputPointValues, inputLabelValues;

    for (const auto& point : points) {
      if (point.x < 0 || point.x >= imgWidth || point.y < 0 || point.y >= imgHeight) {
        std::cerr << "Invalid point in positive points list: (" << point.x << ", " << point.y << ")\n";
        return;
      }
    }
    for (const auto& point : negativePoints) {
      if (point.x < 0 || point.x >= imgWidth || point.y < 0 || point.y >= imgHeight) {
        std::cerr << "Invalid point in negative points list: (" << point.x << ", " << point.y << ")\n";
        return;
      }
    }
    if (!roi.empty()) {
      if (roi.x < 0 || roi.y < 0 || roi.width <= 0 || roi.height <= 0 || roi.br().x >= imgWidth ||
          roi.br().y >= imgHeight) {
        std::cerr << "Invalid ROI: (" << roi.x << ", " << roi.y << ", " << roi.width << ", "
                  << roi.height << ")\n";
        return;
      }
    }

    for (const auto& point : points) {
      if (point.x > 0 && point.x < imgWidth && point.y > 0 && point.y < imgHeight) {
        inputPointValues.emplace_back(static_cast<float>(point.x));
        inputPointValues.emplace_back(static_cast<float>(point.y));
        inputLabelValues.emplace_back(1);
      }
    }
    for (const auto& point : negativePoints) {
      if (point.x > 0 && point.x < imgWidth && point.y > 0 && point.y < imgHeight) {
        inputPointValues.emplace_back(static_cast<float>(point.x));
        inputPointValues.emplace_back(static_cast<float>(point.y));
        inputLabelValues.emplace_back(0);
      }
    }

    if (!roi.empty()) {
      if (roi.width > 0 && roi.height > 0 && roi.x > 0 && roi.x < imgWidth && roi.y > 0 &&
          roi.y < imgHeight && roi.br().x > 0 && roi.br().x < imgWidth && roi.br().y > 0 &&
          roi.br().y < imgHeight) {
        inputPointValues.emplace_back(static_cast<float>(roi.x));
        inputPointValues.emplace_back(static_cast<float>(roi.y));
        inputLabelValues.emplace_back(2);
        inputPointValues.emplace_back(static_cast<float>(roi.br().x));
        inputPointValues.emplace_back(static_cast<float>(roi.br().y));
        inputLabelValues.emplace_back(3);
      }
    }

    const int numPoints = inputLabelValues.size();
    std::vector<int64_t> inputPointShape = {1, numPoints, 2}, pointLabelsShape = {1, numPoints},
                         maskInputShape = {1, 1, 256, 256}, hasMaskInputShape = {1},
                         origImSizeShape = {2};

    std::vector<Ort::Value> inputTensorsSam;
    try {
      inputTensorsSam.emplace_back(Ort::Value::CreateTensor<float>(
          memoryInfo, (float*)outputTensorValuesPre.data(), outputTensorValuesPre.size(),
          outputShapePre.data(), outputShapePre.size()));

      auto inputNames = inputNamesSam, outputNames = outputNamesSam;
      int outputNumber = 3, outputMaskIndex = 0, outputIOUIndex = 1;
      if (bSamHQ) {
        inputTensorsSam.emplace_back(Ort::Value::CreateTensor<float>(
            memoryInfo, (float*)intermTensorValuesPre.data(), intermTensorValuesPre.size(),
            intermShapePre.data(), intermShapePre.size()));
        inputNames = inputNamesSamHQ;
      } else if (bEdgeSam) {
        outputNames = outputNamesEdgeSam;
        outputNumber = 2;
        outputMaskIndex = 1;
        outputIOUIndex = 0;
      }

      if (inputPointValues.size() != 2 * numPoints || inputLabelValues.size() != numPoints) {
        std::cerr << "Mismatch in input points or labels size.\n";
        return;
      }


      inputTensorsSam.emplace_back(
          Ort::Value::CreateTensor<float>(memoryInfo, inputPointValues.data(), 2 * numPoints,
                                          inputPointShape.data(), inputPointShape.size()));
      inputTensorsSam.emplace_back(
          Ort::Value::CreateTensor<float>(memoryInfo, inputLabelValues.data(),
                                                                numPoints, pointLabelsShape.data(),
                                                                pointLabelsShape.size()));

      if (!bEdgeSam) {
        inputTensorsSam.emplace_back(
            Ort::Value::CreateTensor<float>(memoryInfo, maskInputValues, maskInputSize,
                                            maskInputShape.data(), maskInputShape.size()));
        inputTensorsSam.emplace_back(Ort::Value::CreateTensor<float>(
            memoryInfo, hasMaskValues, 1, hasMaskInputShape.data(), hasMaskInputShape.size()));
        inputTensorsSam.emplace_back(Ort::Value::CreateTensor<float>(
            memoryInfo, orig_im_size_values, 2, origImSizeShape.data(), origImSizeShape.size()));
      }

      if (outputMaskSam.type() != CV_8UC1 ||
          outputMaskSam.size() != cv::Size(inputShapePre[3], inputShapePre[2])) {
        outputMaskSam = cv::Mat(inputShapePre[2], inputShapePre[3], CV_8UC1);
      }

      Ort::RunOptions runOptionsSam;
      auto outputTensorsSam = sessionSam->Run(runOptionsSam, inputNames, inputTensorsSam.data(),
                                              inputTensorsSam.size(), outputNames, outputNumber);

      if (outputTensorsSam.size() < 2 || !outputTensorsSam[outputMaskIndex].IsTensor() || !outputTensorsSam[outputIOUIndex].IsTensor()) {
        std::cerr << "Output tensors are missing or not tensors.\n";
        return;
      }


      auto& outputMask = outputTensorsSam[outputMaskIndex];
      auto maskShape = outputMask.GetTensorTypeAndShapeInfo().GetShape();

      cv::Mat outputMaskImage(maskShape[2], maskShape[3], CV_32FC1,
                              outputMask.GetTensorMutableData<float>());
      if (outputMaskImage.size() != outputMaskSam.size()) {
        cv::resize(outputMaskImage, outputMaskImage, outputMaskSam.size());
      }

      for (int i = 0; i < outputMaskSam.rows; i++) {
        for (int j = 0; j < outputMaskSam.cols; j++) {
          outputMaskSam.at<uint8_t>(i, j) = outputMaskImage.at<float>(i, j) > 0 ? 255 : 0;
        }
      }

      if (outputTensorsSam.size() > 1 &&
          outputTensorsSam[1].GetTensorTypeAndShapeInfo().GetElementCount() > 0) {
        iouValue = outputTensorsSam[outputIOUIndex].GetTensorMutableData<float>()[0];
      } else {
        std::cerr << "____sam_cpp_lib error message!!!____ IOU tensor is missing or empty"
                  << std::endl;
        iouValue = 0.0;  // default value in case of error
      }
    } catch (const Ort::Exception& e) {
      std::cerr << "____sam_cpp_lib error message!!!____ ONNX Runtime exception: " << e.what()
                << std::endl;
      throw;
    } catch (const std::exception& e) {
      std::cerr << "____sam_cpp_lib error message!!!____ Standard exception: " << e.what()
                << std::endl;
      throw;
    } catch (...) {
      std::cerr << "____sam_cpp_lib error message!!!____ Unknown exception" << std::endl;
      throw;
    }
  }
};

Sam::Sam(const std::string& preModelPath, const std::string& samModelPath, int threadsNumber)
    : Sam(Parameter(preModelPath, samModelPath, threadsNumber)) {}

Sam::Sam(const Parameter& param) : m_model(new SamModel(param)) {}

Sam::~Sam() { delete m_model; }

cv::Size Sam::getInputSize() const { return m_model->getInputSize(); }
bool Sam::loadImage(const cv::Mat& image) { return m_model->loadImage(image); }

cv::Mat Sam::getMask(const cv::Point& point, double* iou) const {
  return getMask({point}, {}, {}, iou);
}

cv::Mat Sam::getMask(const std::list<cv::Point>& points, const std::list<cv::Point>& negativePoints,
                     double* iou) const {
  return getMask(points, negativePoints, {}, iou);
}

cv::Mat Sam::getMask(const std::list<cv::Point>& points, const std::list<cv::Point>& negativePoints,
                     const cv::Rect& roi, double* iou) const {
  double iouValue = 0;
  cv::Mat m;
  m = cv::Mat::zeros(m_model->getInputSize().height, m_model->getInputSize().width, CV_8UC1);
  m_model->getMask(points, negativePoints, roi, m, iouValue);
  if (iou != nullptr) {
    *iou = iouValue;
  }
  return m;
}

// Just a poor version of
// https://github.com/facebookresearch/segment-anything/blob/main/notebooks/automatic_mask_generator_example.ipynb
cv::Mat Sam::autoSegment(const cv::Size& numPoints, cbProgress cb, const double iouThreshold,
                         const double minArea, int* numObjects) const {
  if (numPoints.empty()) {
    return {};
  }

  const auto size = getInputSize();
  cv::Mat mask, outImage = cv::Mat::zeros(size, CV_64FC1);

  std::vector<double> masksAreas;

  for (int i = 0; i < numPoints.height; i++) {
    for (int j = 0; j < numPoints.width; j++) {
      if (cb) {
        cb(double(i * numPoints.width + j) / (numPoints.width * numPoints.height));
      }

      cv::Point input(cv::Point((j + 0.5) * size.width / numPoints.width,
                                (i + 0.5) * size.height / numPoints.height));

      double iou;
      m_model->getMask({input}, {}, {}, mask, iou);
      if (mask.empty() || iou < iouThreshold) {
        continue;
      }

      std::vector<std::vector<cv::Point>> contours;
      cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
      if (contours.empty()) {
        continue;
      }

      int maxContourIndex = 0;
      double maxContourArea = 0;
      for (int i = 0; i < contours.size(); i++) {
        double area = cv::contourArea(contours[i]);
        if (area > maxContourArea) {
          maxContourArea = area;
          maxContourIndex = i;
        }
      }
      if (maxContourArea < minArea) {
        continue;
      }

      cv::Mat contourMask = cv::Mat::zeros(size, CV_8UC1);
      cv::drawContours(contourMask, contours, maxContourIndex, cv::Scalar(255), cv::FILLED);
      cv::Rect boundingBox = cv::boundingRect(contours[maxContourIndex]);

      int index = masksAreas.size() + 1, numPixels = 0;
      for (int i = boundingBox.y; i < boundingBox.y + boundingBox.height; i++) {
        for (int j = boundingBox.x; j < boundingBox.x + boundingBox.width; j++) {
          if (contourMask.at<uchar>(i, j) == 0) {
            continue;
          }

          auto dst = (int)outImage.at<double>(i, j);
          if (dst > 0 && masksAreas[dst - 1] < maxContourArea) {
            continue;
          }
          outImage.at<double>(i, j) = index;
          numPixels++;
        }
      }
      if (numPixels == 0) {
        continue;
      }

      masksAreas.emplace_back(maxContourArea);
    }
  }
  return outImage;
}
