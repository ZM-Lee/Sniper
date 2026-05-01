#ifndef SNIPER_ENCODER_HPP_
#define SNIPER_ENCODER_HPP_

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <opencv2/opencv.hpp>

#include "sender/mqtt.hpp"
#include "sender/serial.hpp"

namespace doorlock_sniper
{

class VideoEncoder
{
public:
  struct Options
  {
    std::string send_chain = "serial";
    int crop_size = 800;
    int output_size = 400;
    int output_fps = 50;
    int target_bitrate = 40;
    int packet_size = 300;
    bool static_simplify = true;
    int motion_threshold = 14;
    int motion_erode_px = 1;
    int motion_dilate_px = 2;
    int motion_trail_frames = 3;
    double trail_disable_motion_ratio = 0.30;
    double bg_update_alpha = 0.01;
    double bg_blur_sigma = 1.2;
    int center_clear_size = 100;
    bool force_monochrome = false;
    double bandwidth_limit_kbytes = 14.0;
    double bandwidth_window_s = 2.0;
    double max_tx_delay_s = 1.0;
    bool enable_display = true;
    bool debug_dump_enable = false;
    int debug_dump_every_n_frames = 20;
    bool debug_dump_save_raw = true;
    bool debug_dump_save_roi = true;
    bool debug_dump_save_static = true;
    bool debug_dump_save_final = true;

    std::string serial_port = "/dev/ttyUSB0";
    int serial_baudrate = 921600;
    int serial_cmd_id = 0x0310;

    std::string mqtt_host = "192.168.50.78";
    int mqtt_port = 3333;
    std::string mqtt_topic = "CustomByteBlock";
    std::string mqtt_client_id = "doorlock_sniper";
    int mqtt_qos = 1;

    std::string x264_preset = "auto";
    std::string debug_dump_dir = "sniper_debug_imgs";
  };

  explicit VideoEncoder(Options options);
  ~VideoEncoder();

  bool processImage(const cv::Mat & input, int64_t timestamp_ns = 0, uint64_t source_frame_id = 0);

private:
  struct PendingInputFrame
  {
    uint64_t source_frame_id = 0;
    int64_t input_time_ns = 0;
    int64_t enqueue_time_ns = 0;
  };

  struct ByteSpan
  {
    uint64_t track_id = 0;
    size_t bytes = 0;
  };

  struct TxPacket
  {
    std::vector<uint8_t> payload;
    std::vector<ByteSpan> spans;
  };

  struct FrameTrack
  {
    uint64_t track_id = 0;
    uint64_t source_frame_id = 0;
    int64_t input_time_ns = 0;
    int64_t enqueue_time_ns = 0;
    size_t encoded_bytes = 0;
    size_t sent_bytes = 0;
    size_t dropped_bytes = 0;
  };

  static int64_t nowNs();

  uint64_t reserveFrameToken(int64_t input_time_ns, uint64_t source_frame_id);
  void initialize_gstreamer();
  void shutdown_gstreamer();

  cv::Mat preprocess_image(
    const cv::Mat & input,
    cv::Mat * roi_downsample = nullptr,
    cv::Mat * static_removed = nullptr);

  void push_frame_to_gstreamer(const cv::Mat & frame, uint64_t frame_token);
  void pull_stream_and_packetize();
  void poll_gstreamer_bus();
  void watchdog_gstreamer_pipeline(int64_t now_ns);
  void restart_gstreamer_pipeline(const char * reason);
  void tx_loop();
  void display_loop();
  void appendStreamSpan(std::deque<ByteSpan> & spans, uint64_t track_id, size_t bytes);
  void consumeStreamSpans(
    std::deque<ByteSpan> & spans,
    size_t bytes_to_consume,
    std::vector<ByteSpan> * consumed_spans);
  void accountConsumedSpans(const std::vector<ByteSpan> & spans, bool sent, int64_t completion_ns);
  void maybeLogLatencyStats(int64_t now_ns);
  void maybeLogRuntimeStats(int64_t now_ns);
  void retireFrameTrackIfDone(uint64_t track_id);

  Options options_;

  GstElement * pipeline_;
  GstElement * appsrc_;
  GstElement * appsink_;
  GstBus * bus_;

  std::unique_ptr<sniper::sender::UsbCustomByteBlockSender> serial_sender_;
  std::unique_ptr<sniper::sender::MqttCustomByteBlockSender> mqtt_sender_;

  uint64_t packet_sequence_id_ = 0;
  uint32_t frame_count_ = 0;

  std::thread display_thread_;
  std::atomic<bool> display_running_;

  std::mutex frame_mutex_;
  cv::Mat display_raw_frame_;
  cv::Mat display_roi_frame_;
  cv::Mat display_static_frame_;
  cv::Mat display_frame_;

  std::vector<uint8_t> stream_buffer_;
  std::deque<ByteSpan> stream_buffer_spans_;
  std::deque<TxPacket> tx_queue_;
  std::deque<std::pair<int64_t, size_t>> sent_window_;
  std::deque<cv::Mat> motion_mask_history_;
  std::deque<cv::Mat> trail_frame_history_;
  std::unordered_map<uint64_t, PendingInputFrame> pending_input_frames_;
  std::unordered_map<uint64_t, FrameTrack> active_frame_tracks_;
  size_t sent_window_bytes_ = 0;
  uint64_t dropped_bytes_ = 0;
  uint32_t dropped_events_ = 0;
  int64_t last_telemetry_ns_ = 0;
  int64_t last_latency_report_ns_ = 0;
  int64_t last_runtime_report_ns_ = 0;
  std::mutex buffer_mutex_;
  std::condition_variable tx_cv_;

  std::thread tx_thread_;
  std::atomic<bool> tx_running_{false};
  int64_t next_tx_deadline_ns_ = 0;
  int64_t frame_interval_ns_ = 0;
  int64_t stream_buffer_first_byte_ns_ = 0;
  int64_t pipeline_started_ns_ = 0;
  int64_t last_input_frame_ns_ = 0;
  int64_t last_encoded_sample_ns_ = 0;
  int64_t last_watchdog_log_ns_ = 0;
  uint64_t next_input_frame_token_ = 1;
  uint64_t next_frame_track_id_ = 1;
  uint8_t payload_data_seq_ = 0;
  uint64_t input_frame_count_ = 0;
  uint64_t throttled_frame_count_ = 0;
  uint64_t pushed_frame_count_ = 0;
  uint64_t push_failed_count_ = 0;
  uint64_t pulled_sample_count_ = 0;
  uint64_t sent_packet_count_ = 0;
  uint64_t send_failed_count_ = 0;
  uint64_t latency_sample_count_ = 0;
  double latency_sum_ms_ = 0.0;
  double latency_latest_ms_ = 0.0;
  double latency_min_ms_ = -1.0;
  double latency_max_ms_ = 0.0;
  uint64_t latency_latest_frame_id_ = 0;

  int64_t last_encode_stamp_ns_ = 0;
  uint64_t display_frame_counter_ = 0;
  cv::Mat background_gray_f32_;
  cv::Mat motion_erode_kernel_;
  cv::Mat motion_dilate_kernel_;
};

}  // namespace doorlock_sniper

#endif  // SNIPER_ENCODER_HPP_
