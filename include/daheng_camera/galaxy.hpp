#ifndef SNIPER_DAHENG_CAMERA_GALAXY_HPP_
#define SNIPER_DAHENG_CAMERA_GALAXY_HPP_

#include <cstdint>
#include <string>
#include <vector>

#include <opencv2/core/mat.hpp>

#include "daheng_camera/driver/GxIAPI.h"

namespace sniper::daheng_camera
{

class GalaxyCamera
{
public:
  struct FrameInfo
  {
    uint64_t frame_id = 0;
    uint64_t camera_timestamp_ticks = 0;
    int64_t capture_time_ns = 0;
    int64_t host_receive_time_ns = 0;
    bool capture_time_valid = false;
    bool hardware_timestamp_valid = false;
  };

  struct Config
  {
    double exposure_time = 4000.0;
    double gain = 24.0;
    int width = 1920;
    int height = 1080;
    double acquisition_fps = 60.0;
    bool balance_white_auto = true;
    bool trigger_mode = false;
  };

  GalaxyCamera();
  ~GalaxyCamera();

  bool open(const Config & config);
  bool openFromYaml(const std::string & yaml_path);
  bool read(cv::Mat & bgr_image, FrameInfo * frame_info = nullptr, int timeout_ms = 100);
  void close();

  bool isOpened() const;
  int width() const;
  int height() const;

private:
  void applyConfig(const Config & config);
  void selectBayerFormat();
  void calibrateTimestampClock();
  int64_t cameraTicksToHostNs(uint64_t camera_ticks) const;

  GX_DEV_HANDLE device_handle_;
  int64_t payload_size_;
  int width_;
  int height_;
  int bayer_format_index_;
  bool lib_initialized_;
  bool stream_on_;
  bool timestamp_calibrated_;
  uint64_t timestamp_tick_frequency_;
  uint64_t timestamp_base_camera_ticks_;
  int64_t timestamp_base_host_ns_;
  std::vector<unsigned char> frame_buffer_;
};

}  // namespace sniper::daheng_camera

#endif  // SNIPER_DAHENG_CAMERA_GALAXY_HPP_
