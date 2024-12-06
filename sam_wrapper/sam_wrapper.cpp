#include "sam.h"
#include "sam_wrapper.h"
#include <opencv2/opencv.hpp>

std::shared_ptr<Sam> m_sam;  // 使用 shared_ptr 来管理 Sam 对象
cv::Mat m_SamMask;
std::mutex m_SamMutex;

SamWrapper::SamWrapper(const std::string& preModelPath, const std::string& samModelPath, int threadsNumber) {
  //Sam::Parameter param(preModelPath, samModelPath, threadsNumber);
  //m_sam = std::make_shared<Sam>(param);
  m_sam = std::make_shared<Sam>(preModelPath, samModelPath, threadsNumber);
}

SamWrapper::~SamWrapper() {
}

cv::Size SamWrapper::getInputSize() const { return m_sam->getInputSize(); }

bool SamWrapper::loadImage(const cv::Mat& image) { 
    const cv::Mat& _image = image.clone();
    return m_sam->loadImage(_image); 
}

cv::Mat SamWrapper::getMask(const std::list<cv::Point>& points, const std::list<cv::Point>& negativePoints, const cv::Rect& roi, double* iou) const {
  m_SamMutex.lock();
  try {
    const std::list<cv::Point> _points(points.begin(), points.end());
    const std::list<cv::Point> _negativePoints(negativePoints.begin(), negativePoints.end());
    const cv::Rect& _roi = roi;
    m_SamMask = m_sam->getMask(_points, _negativePoints, _roi);
  } catch (const std::exception& e) {
    std::cerr << "Error occurred: " << e.what() << std::endl;
    m_SamMask = cv::Mat::zeros(m_sam->getInputSize().height, m_sam->getInputSize().width, CV_8UC1);
  }
  m_SamMutex.unlock();
  return m_SamMask;
}
