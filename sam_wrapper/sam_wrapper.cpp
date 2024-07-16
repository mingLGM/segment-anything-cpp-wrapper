#include "sam.h"
#include "sam_wrapper.h"
#include <opencv2/opencv.hpp>

std::shared_ptr<Sam> m_sam;  // 使用 shared_ptr 来管理 Sam 对象

SamWrapper::SamWrapper(const std::string& preModelPath, const std::string& samModelPath, int threadsNumber) {
  //Sam::Parameter param(preModelPath, samModelPath, threadsNumber);
  //m_sam = std::make_shared<Sam>(param);
  m_sam = std::make_shared<Sam>(preModelPath, samModelPath, threadsNumber);
}

SamWrapper::~SamWrapper() {
}

cv::Size SamWrapper::getInputSize() const { return m_sam->getInputSize(); }

bool SamWrapper::loadImage(const cv::Mat& image) { return m_sam->loadImage(image); }

cv::Mat SamWrapper::getMask(const std::list<cv::Point>& points, const std::list<cv::Point>& negativePoints, const cv::Rect& roi, double* iou) const {
  SamMutex.lock();
  try {
    SamMask = m_sam->getMask(points, negativePoints, roi);
  } catch (const std::exception& e) {
    std::cerr << "Error occurred: " << e.what() << std::endl;
    SamMask = cv::Mat::zeros(m_sam->getInputSize().height, m_sam->getInputSize().width, CV_8UC1);
  }
  SamMutex.unlock();
  return SamMask;
}
