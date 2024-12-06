#ifndef SAM_WRAPPER_H_
#define SAM_WRAPPER_H_

#include <memory>  
#include <opencv2/core.hpp>
#include <list>

class __declspec(dllexport) SamWrapper {
 public:
  SamWrapper(const std::string& preModelPath, const std::string& samModelPath, int threadsNumber);
  ~SamWrapper();
  cv::Size getInputSize() const;
  bool loadImage(const cv::Mat& image);
  cv::Mat getMask(const std::list<cv::Point>& points, const std::list<cv::Point>& negativePoints,
                  const cv::Rect& roi, double* iou = nullptr) const;
};

  // 使用 typedef 定义指针类型
typedef std::shared_ptr<SamWrapper> SAM;

#endif  // SAM_WRAPPER_H_
