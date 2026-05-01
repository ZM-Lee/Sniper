#include "encoder.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace doorlock_sniper
{
namespace
{

void LogInfo(const std::string & msg)
{
  std::cout << "[Encoder][INFO] " << msg << std::endl;
}

void LogWarn(const std::string & msg)
{
  std::cerr << "[Encoder][WARN] " << msg << std::endl;
}

void LogError(const std::string & msg)
{
  std::cerr << "[Encoder][ERROR] " << msg << std::endl;
}

}  // namespace

int64_t VideoEncoder::nowNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
           std::chrono::steady_clock::now().time_since_epoch())
    .count();
}

uint64_t VideoEncoder::reserveFrameToken(const int64_t input_time_ns, const uint64_t source_frame_id)
{
  std::lock_guard<std::mutex> lock(buffer_mutex_);

  const uint64_t frame_token = next_input_frame_token_++;
  PendingInputFrame pending;
  pending.source_frame_id = (source_frame_id != 0U) ? source_frame_id : frame_token;
  pending.input_time_ns = input_time_ns;
  pending.enqueue_time_ns = nowNs();
  pending_input_frames_[frame_token] = pending;
  return frame_token;
}

VideoEncoder::VideoEncoder(Options options)
: options_(std::move(options)),
  pipeline_(nullptr),
  appsrc_(nullptr),
  appsink_(nullptr),
  bus_(nullptr),
  display_running_(false)
{
  constexpr int kVideoPacketBytes = 300;
  constexpr int kVideoDataBytes = 150;
  constexpr int kFixedOutputFps = 50;

  if (options_.output_fps != kFixedOutputFps) {
    LogWarn("output_fps forced to fixed 50Hz");
  }
  options_.output_fps = kFixedOutputFps;
  if (options_.packet_size != kVideoPacketBytes) {
    LogWarn("packet_size overridden to fixed 300 bytes");
    options_.packet_size = kVideoPacketBytes;
  }
  if ((kVideoDataBytes + 3) > options_.packet_size) {
    throw std::runtime_error(
      "Invalid packet split, packet size must reserve 1 byte for seq and 2 bytes for pad count");
  }
  if (options_.motion_trail_frames < 0) {
    options_.motion_trail_frames = 0;
  }
  if (options_.motion_trail_frames > 15) {
    options_.motion_trail_frames = 15;
  }
  options_.trail_disable_motion_ratio = std::clamp(options_.trail_disable_motion_ratio, 0.0, 1.0);
  options_.motion_erode_px = std::clamp(options_.motion_erode_px, 0, 20);
  options_.motion_dilate_px = std::clamp(options_.motion_dilate_px, 0, 20);
  options_.bandwidth_limit_kbytes = std::max(1.0, options_.bandwidth_limit_kbytes);
  options_.bandwidth_window_s = std::max(0.2, options_.bandwidth_window_s);
  options_.max_tx_delay_s = std::max(0.05, options_.max_tx_delay_s);
  options_.debug_dump_every_n_frames = std::max(1, options_.debug_dump_every_n_frames);
  frame_interval_ns_ = 1000000000LL / std::max(options_.output_fps, 1);

  if (options_.debug_dump_enable) {
    const bool any_encoder_save =
      options_.debug_dump_save_raw || options_.debug_dump_save_roi ||
      options_.debug_dump_save_static || options_.debug_dump_save_final;
    if (!any_encoder_save) {
      LogWarn("debug dump enabled but all dump switches are off");
    } else {
      const std::filesystem::path dump_dir = std::filesystem::path(options_.debug_dump_dir) / "encoder";
      std::error_code ec;
      std::filesystem::create_directories(dump_dir, ec);
      if (ec) {
        LogWarn("Create debug dump dir failed, disable debug dump");
        options_.debug_dump_enable = false;
      }
    }
  }

  std::string send_chain = options_.send_chain;
  std::transform(
    send_chain.begin(), send_chain.end(), send_chain.begin(),
    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  options_.send_chain = send_chain;

  if (options_.send_chain == "serial") {
    serial_sender_ = std::make_unique<sniper::sender::UsbCustomByteBlockSender>(
      options_.serial_port,
      options_.serial_baudrate,
      static_cast<uint16_t>(options_.serial_cmd_id & 0xFFFF));

    if (!serial_sender_->open()) {
      throw std::runtime_error("Failed to open serial port");
    }
  } else if (options_.send_chain == "mqtt") {
    mqtt_sender_ = std::make_unique<sniper::sender::MqttCustomByteBlockSender>(
      options_.mqtt_host,
      options_.mqtt_port,
      options_.mqtt_topic,
      options_.mqtt_client_id,
      options_.mqtt_qos);

    if (!mqtt_sender_->open()) {
      throw std::runtime_error("Failed to open MQTT client: " + mqtt_sender_->lastError());
    }
  } else {
    throw std::runtime_error("Unsupported send_chain, use serial or mqtt");
  }

  initialize_gstreamer();

  if (options_.enable_display) {
    display_running_ = true;
    display_thread_ = std::thread(&VideoEncoder::display_loop, this);
  }

  std::ostringstream oss;
  oss << "Encoder initialized: " << options_.output_size << "x" << options_.output_size
      << "@" << options_.output_fps << "fps bitrate=" << options_.target_bitrate
      << " chain=" << options_.send_chain;
  if (options_.send_chain == "serial") {
    oss << " serial=" << options_.serial_port << "@" << options_.serial_baudrate;
  } else {
    oss << " mqtt=" << options_.mqtt_host << ":" << options_.mqtt_port
        << " topic=" << options_.mqtt_topic;
  }
  LogInfo(oss.str());

  tx_running_ = true;
  tx_thread_ = std::thread(&VideoEncoder::tx_loop, this);
}

VideoEncoder::~VideoEncoder()
{
  tx_running_ = false;
  tx_cv_.notify_all();
  if (tx_thread_.joinable()) {
    tx_thread_.join();
  }

  if (options_.enable_display) {
    display_running_ = false;
    if (display_thread_.joinable()) {
      display_thread_.join();
    }
    cv::destroyAllWindows();
  }

  if (serial_sender_) {
    serial_sender_->close();
  }
  if (mqtt_sender_) {
    mqtt_sender_->close();
  }

  shutdown_gstreamer();
}

void VideoEncoder::initialize_gstreamer()
{
  gst_init(nullptr, nullptr);

  pipeline_ = gst_pipeline_new("encoder_pipe");
  appsrc_ = gst_element_factory_make("appsrc", "source");
  appsink_ = gst_element_factory_make("appsink", "sink");
  GstElement * convert = gst_element_factory_make("videoconvert", "convert");
  GstElement * encoder = gst_element_factory_make("x264enc", "encoder");
  GstElement * parser = gst_element_factory_make("h264parse", "parser");

  if (!pipeline_ || !appsrc_ || !appsink_ || !convert || !encoder || !parser) {
    throw std::runtime_error("GStreamer element creation failed");
  }

  GstCaps * caps = gst_caps_new_simple(
    "video/x-raw",
    "format", G_TYPE_STRING, "BGR",
    "width", G_TYPE_INT, options_.output_size,
    "height", G_TYPE_INT, options_.output_size,
    "framerate", GST_TYPE_FRACTION, options_.output_fps, 1,
    nullptr);

  g_object_set(
    G_OBJECT(appsrc_),
    "caps", caps,
    "block", FALSE,
    "max-bytes", static_cast<guint64>(options_.output_size * options_.output_size * 3 * 3),
    "stream-type", 0,
    "format", GST_FORMAT_TIME,
    "is-live", TRUE,
    "do-timestamp", FALSE,
    nullptr);
  gst_caps_unref(caps);

  const bool low_bitrate_mode = (options_.target_bitrate <= 80);
  const int key_int = std::max(options_.output_fps, 30);
  const int default_speed_preset = low_bitrate_mode ? 9 : 3;
  int speed_preset = default_speed_preset;

  std::string preset_lower = options_.x264_preset;
  std::transform(
    preset_lower.begin(), preset_lower.end(), preset_lower.begin(),
    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  if (!preset_lower.empty() && preset_lower != "auto") {
    if (preset_lower == "ultrafast") speed_preset = 1;
    else if (preset_lower == "superfast") speed_preset = 2;
    else if (preset_lower == "veryfast") speed_preset = 3;
    else if (preset_lower == "faster") speed_preset = 4;
    else if (preset_lower == "fast") speed_preset = 5;
    else if (preset_lower == "medium") speed_preset = 6;
    else if (preset_lower == "slow") speed_preset = 7;
    else if (preset_lower == "slower") speed_preset = 8;
    else if (preset_lower == "veryslow") speed_preset = 9;
    else if (preset_lower == "placebo") speed_preset = 10;
    else speed_preset = default_speed_preset;
  }

  if (low_bitrate_mode) {
    g_object_set(
      G_OBJECT(encoder),
      "bitrate", options_.target_bitrate,
      "speed-preset", speed_preset,
      "tune", 0x00000004,
      "byte-stream", TRUE,
      "key-int-max", options_.output_fps,
      "bframes", 0,
      "rc-lookahead", 0,
      "sync-lookahead", 0,
      "sliced-threads", TRUE,
      "ref", 1,
      "aud", TRUE,
      "vbv-buf-capacity", 500,
      "option-string", "repeat-headers=1:scenecut=0:ref=1:force-cfr=1",
      "pass", 0,
      nullptr);
  } else {
    g_object_set(
      G_OBJECT(encoder),
      "bitrate", options_.target_bitrate,
      "speed-preset", speed_preset,
      "tune", 0x00000004,
      "byte-stream", TRUE,
      "key-int-max", options_.output_fps,
      "bframes", 0,
      "rc-lookahead", 0,
      "sync-lookahead", 0,
      "sliced-threads", TRUE,
      "aud", TRUE,
      "option-string", "repeat-headers=1:scenecut=0:ref=1:force-cfr=1",
      "pass", 0,
      nullptr);
  }

  g_object_set(
    G_OBJECT(parser),
    "config-interval", -1,
    "disable-passthrough", TRUE,
    nullptr);

  GstCaps * h264_caps = gst_caps_new_simple(
    "video/x-h264",
    "stream-format", G_TYPE_STRING, "byte-stream",
    "alignment", G_TYPE_STRING, "au",
    nullptr);

  g_object_set(
    G_OBJECT(appsink_),
    "caps", h264_caps,
    "max-buffers", 5,
    "drop", FALSE,
    "emit-signals", FALSE,
    "sync", FALSE,
    nullptr);
  gst_caps_unref(h264_caps);

  gst_bin_add_many(GST_BIN(pipeline_), appsrc_, convert, encoder, parser, appsink_, nullptr);
  if (!gst_element_link_many(appsrc_, convert, encoder, parser, appsink_, nullptr)) {
    throw std::runtime_error("GStreamer pipeline link failed");
  }

  if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    throw std::runtime_error("GStreamer pipeline start failed");
  }

  bus_ = gst_element_get_bus(pipeline_);
  pipeline_started_ns_ = nowNs();
  last_encoded_sample_ns_ = 0;
  last_watchdog_log_ns_ = 0;
}

void VideoEncoder::shutdown_gstreamer()
{
  if (pipeline_) {
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (bus_) {
      gst_object_unref(bus_);
      bus_ = nullptr;
    }
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
  }
  appsrc_ = nullptr;
  appsink_ = nullptr;
}

cv::Mat VideoEncoder::preprocess_image(
  const cv::Mat & input,
  cv::Mat * roi_downsample,
  cv::Mat * static_removed)
{
  int x = (input.cols - options_.crop_size) / 2;
  int y = 0;
  x = std::max(0, x);
  y = std::max(0, y);
  const int w = std::min(options_.crop_size, input.cols - x);
  const int h = std::min(options_.crop_size, input.rows - y);

  cv::Mat cropped = input(cv::Rect(x, y, w, h));
  cv::Mat resized;
  cv::resize(cropped, resized, cv::Size(options_.output_size, options_.output_size), 0, 0, cv::INTER_LINEAR);

  if (roi_downsample) {
    resized.copyTo(*roi_downsample);
  }

  cv::Mat working = resized;
  if (options_.force_monochrome) {
    cv::Mat gray_full;
    cv::cvtColor(working, gray_full, cv::COLOR_BGR2GRAY);
    cv::cvtColor(gray_full, working, cv::COLOR_GRAY2BGR);
  }

  if (!options_.static_simplify) {
    if (static_removed) {
      working.copyTo(*static_removed);
    }
    return working;
  }

  cv::Mat gray;
  cv::cvtColor(working, gray, cv::COLOR_BGR2GRAY);
  if (background_gray_f32_.empty()) {
    gray.convertTo(background_gray_f32_, CV_32F);
    return working;
  }

  cv::Mat bg_u8;
  cv::convertScaleAbs(background_gray_f32_, bg_u8);

  cv::Mat diff;
  cv::absdiff(gray, bg_u8, diff);

  cv::Mat motion_mask;
  cv::threshold(diff, motion_mask, options_.motion_threshold, 255, cv::THRESH_BINARY);

  if (options_.motion_erode_px > 0) {
    if (motion_erode_kernel_.empty()) {
      const int k = 2 * options_.motion_erode_px + 1;
      motion_erode_kernel_ = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(k, k));
    }
    cv::erode(motion_mask, motion_mask, motion_erode_kernel_, cv::Point(-1, -1), 1);
  }

  if (options_.motion_dilate_px > 0) {
    if (motion_dilate_kernel_.empty()) {
      const int k = 2 * options_.motion_dilate_px + 1;
      motion_dilate_kernel_ = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(k, k));
    }
    cv::dilate(motion_mask, motion_mask, motion_dilate_kernel_, cv::Point(-1, -1), 1);
  }

  const double motion_ratio_raw =
    static_cast<double>(cv::countNonZero(motion_mask)) / static_cast<double>(motion_mask.total());
  const bool suppress_trail = (motion_ratio_raw >= options_.trail_disable_motion_ratio);

  if (options_.center_clear_size > 0) {
    const int clear_size = std::min({options_.center_clear_size, working.cols, working.rows});
    const int x0 = std::max(0, working.cols / 2 - clear_size / 2);
    const int y0 = 50;
    const int cw = 100;
    const int ch = 200;
    cv::rectangle(motion_mask, cv::Rect(x0, y0, cw, ch), cv::Scalar(255), cv::FILLED);
  }

  cv::Mat static_base = working.clone();
  if (!options_.force_monochrome && options_.target_bitrate <= 80) {
    cv::Mat gray_bg;
    cv::cvtColor(static_base, gray_bg, cv::COLOR_BGR2GRAY);
    cv::cvtColor(gray_bg, static_base, cv::COLOR_GRAY2BGR);
  }

  cv::Mat blurred_static;
  cv::GaussianBlur(
    static_base,
    blurred_static,
    cv::Size(),
    std::max(0.0, options_.bg_blur_sigma),
    std::max(0.0, options_.bg_blur_sigma));

  cv::Mat focused = blurred_static.clone();
  working.copyTo(focused, motion_mask);
  if (static_removed) {
    focused.copyTo(*static_removed);
  }

  if (options_.motion_trail_frames > 0) {
    motion_mask_history_.push_back(motion_mask.clone());
    trail_frame_history_.push_back(working.clone());
    const size_t max_history = static_cast<size_t>(options_.motion_trail_frames + 1);
    while (motion_mask_history_.size() > max_history) {
      motion_mask_history_.pop_front();
    }
    while (trail_frame_history_.size() > max_history) {
      trail_frame_history_.pop_front();
    }

    const size_t history_size = motion_mask_history_.size();
    if (!suppress_trail && history_size > 1 && history_size == trail_frame_history_.size()) {
      cv::Mat trail_mask = motion_mask.clone();
      cv::Mat trail_img = working.clone();
      for (size_t i = 0; i < history_size - 1; ++i) {
        cv::bitwise_or(trail_mask, motion_mask_history_[i], trail_mask);
        cv::max(trail_img, trail_frame_history_[i], trail_img);
      }
      trail_img.copyTo(focused, trail_mask);
    }
  } else {
    motion_mask_history_.clear();
    trail_frame_history_.clear();
  }

  cv::accumulateWeighted(gray, background_gray_f32_, std::clamp(options_.bg_update_alpha, 0.001, 0.2));
  return focused;
}

bool VideoEncoder::processImage(const cv::Mat & input, const int64_t timestamp_ns, const uint64_t source_frame_id)
{
  if (input.empty()) {
    return false;
  }

  ++input_frame_count_;
  int64_t now = (timestamp_ns > 0) ? timestamp_ns : nowNs();
  if (last_encode_stamp_ns_ > 0 && now <= last_encode_stamp_ns_) {
    now = nowNs();
  }
  if (last_encode_stamp_ns_ > 0 && (now - last_encode_stamp_ns_) < frame_interval_ns_) {
    ++throttled_frame_count_;
    maybeLogRuntimeStats(nowNs());
    return true;
  }
  last_encode_stamp_ns_ = now;
  last_input_frame_ns_ = nowNs();

  cv::Mat roi_downsample;
  cv::Mat static_removed;
  cv::Mat processed = preprocess_image(input, &roi_downsample, &static_removed);

  if (options_.enable_display) {
    cv::Mat raw_preview;
    cv::resize(
      input,
      raw_preview,
      cv::Size(std::max(1, input.cols / 2), std::max(1, input.rows / 2)),
      0,
      0,
      cv::INTER_AREA);

    std::lock_guard<std::mutex> lock(frame_mutex_);
    raw_preview.copyTo(display_raw_frame_);
    roi_downsample.copyTo(display_roi_frame_);
    static_removed.copyTo(display_static_frame_);
    processed.copyTo(display_frame_);
  }

  const uint64_t frame_token = reserveFrameToken(now, source_frame_id);
  push_frame_to_gstreamer(processed, frame_token);
  pull_stream_and_packetize();
  poll_gstreamer_bus();
  watchdog_gstreamer_pipeline(nowNs());
  maybeLogRuntimeStats(nowNs());
  frame_count_++;
  return true;
}

void VideoEncoder::push_frame_to_gstreamer(const cv::Mat & frame, const uint64_t frame_token)
{
  if (!appsrc_ || frame.empty()) {
    return;
  }

  const size_t size = frame.total() * frame.elemSize();
  GstBuffer * buffer = gst_buffer_new_allocate(nullptr, size, nullptr);

  GstMapInfo map;
  if (gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
    memcpy(map.data, frame.data, size);
    gst_buffer_unmap(buffer, &map);

    if (frame_interval_ns_ > 0) {
      const GstClockTime pts = static_cast<GstClockTime>((frame_token - 1U) * static_cast<uint64_t>(frame_interval_ns_));
      GST_BUFFER_PTS(buffer) = pts;
      GST_BUFFER_DTS(buffer) = pts;
      GST_BUFFER_DURATION(buffer) = static_cast<GstClockTime>(frame_interval_ns_);
    }

    GstFlowReturn ret;
    g_signal_emit_by_name(appsrc_, "push-buffer", buffer, &ret);
    if (ret != GST_FLOW_OK) {
      std::lock_guard<std::mutex> lock(buffer_mutex_);
      pending_input_frames_.erase(frame_token);
      ++push_failed_count_;
      LogWarn("Push buffer failed, flow=" + std::to_string(static_cast<int>(ret)));
    } else {
      ++pushed_frame_count_;
    }
  }
  gst_buffer_unref(buffer);
}

void VideoEncoder::pull_stream_and_packetize()
{
  if (!appsink_) {
    return;
  }

  const bool transport_ready =
    (serial_sender_ && serial_sender_->isOpen()) || (mqtt_sender_ && mqtt_sender_->isOpen());
  if (!transport_ready) {
    return;
  }

  constexpr size_t kVideoDataBytes = 150U;
  const size_t packet_bytes = static_cast<size_t>(options_.packet_size);
  const size_t max_backlog_bytes = static_cast<size_t>(
    options_.bandwidth_limit_kbytes * 1000.0 * options_.max_tx_delay_s);
  const int64_t max_tx_delay_ns = static_cast<int64_t>(options_.max_tx_delay_s * 1000000000.0);
  bool notify_sender = false;

  while (true) {
    GstSample * sample = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink_), 0);
    if (!sample) {
      break;
    }

    GstBuffer * buffer = gst_sample_get_buffer(sample);
    if (!buffer) {
      gst_sample_unref(sample);
      continue;
    }

    GstMapInfo map;
      if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        last_encoded_sample_ns_ = nowNs();
        ++pulled_sample_count_;

        PendingInputFrame pending_input;
        bool pending_found = false;
      const GstClockTime pts = GST_BUFFER_PTS(buffer);
      if (GST_CLOCK_TIME_IS_VALID(pts) && frame_interval_ns_ > 0) {
        const uint64_t frame_token = static_cast<uint64_t>(pts / static_cast<GstClockTime>(frame_interval_ns_)) + 1U;
        const auto pending_it = pending_input_frames_.find(frame_token);
        if (pending_it != pending_input_frames_.end()) {
          pending_input = pending_it->second;
          pending_input_frames_.erase(pending_it);
          pending_found = true;
        }
      }

      const int64_t fallback_input_ns = nowNs();
      const uint64_t track_id = next_frame_track_id_++;
      FrameTrack track;
      track.track_id = track_id;
      track.source_frame_id = pending_found ? pending_input.source_frame_id : track_id;
      track.input_time_ns = pending_found ? pending_input.input_time_ns : fallback_input_ns;
      track.enqueue_time_ns = pending_found ? pending_input.enqueue_time_ns : fallback_input_ns;
      track.encoded_bytes = map.size;
      active_frame_tracks_[track_id] = track;

      const size_t old_size = stream_buffer_.size();
      stream_buffer_.resize(old_size + map.size);
      memcpy(stream_buffer_.data() + old_size, map.data, map.size);
      appendStreamSpan(stream_buffer_spans_, track_id, map.size);
      if (old_size == 0U && map.size > 0U) {
        stream_buffer_first_byte_ns_ = nowNs();
      }

      while (stream_buffer_.size() >= kVideoDataBytes) {
        const size_t valid_data_bytes = kVideoDataBytes;

        // Protocol payload is fixed at 300 bytes; valid bytes are followed by zero padding.
        TxPacket packet;
        packet.payload.resize(packet_bytes, 0U);
        memcpy(packet.payload.data(), stream_buffer_.data(), valid_data_bytes);

        // Store payload seq in the third-to-last byte and zero-padding count in the last 2 bytes.
        packet.payload[packet_bytes - 3] = payload_data_seq_;
        payload_data_seq_ = static_cast<uint8_t>(payload_data_seq_ + 1U);

        const uint16_t pad_zero_count = static_cast<uint16_t>(packet_bytes - valid_data_bytes);
        packet.payload[packet_bytes - 2] = static_cast<uint8_t>(pad_zero_count & 0xFFU);
        packet.payload[packet_bytes - 1] = static_cast<uint8_t>((pad_zero_count >> 8) & 0xFFU);
        consumeStreamSpans(stream_buffer_spans_, valid_data_bytes, &packet.spans);
        tx_queue_.push_back(std::move(packet));
        notify_sender = true;

        memmove(
          stream_buffer_.data(),
          stream_buffer_.data() + valid_data_bytes,
          stream_buffer_.size() - valid_data_bytes);
        stream_buffer_.resize(stream_buffer_.size() - valid_data_bytes);
        if (stream_buffer_.empty()) {
          stream_buffer_first_byte_ns_ = 0;
        } else {
          stream_buffer_first_byte_ns_ = nowNs();
        }
      }

      const size_t queued_bytes = tx_queue_.size() * packet_bytes + stream_buffer_.size();
      if (queued_bytes > max_backlog_bytes) {
        const size_t drop_bytes = queued_bytes - max_backlog_bytes;
        size_t drop_packets = (drop_bytes + packet_bytes - 1U) / packet_bytes;
        drop_packets = std::min(drop_packets, tx_queue_.size());

        if (drop_packets > 0U) {
          for (size_t i = 0; i < drop_packets; ++i) {
            accountConsumedSpans(tx_queue_.front().spans, false, nowNs());
            tx_queue_.pop_front();
          }
          dropped_bytes_ += drop_packets * packet_bytes;
          dropped_events_++;
        }

        if (tx_queue_.empty() && stream_buffer_.size() > max_backlog_bytes) {
          const size_t target_drop = stream_buffer_.size() - max_backlog_bytes;
          size_t raw_drop = target_drop;

          for (size_t i = target_drop; i + 4U < stream_buffer_.size(); ++i) {
            const bool start_code_3 =
              (stream_buffer_[i] == 0U && stream_buffer_[i + 1U] == 0U &&
              stream_buffer_[i + 2U] == 1U);
            const bool start_code_4 =
              (stream_buffer_[i] == 0U && stream_buffer_[i + 1U] == 0U &&
              stream_buffer_[i + 2U] == 0U && stream_buffer_[i + 3U] == 1U);
            if (start_code_3 || start_code_4) {
              raw_drop = i;
              break;
            }
          }

          std::vector<ByteSpan> dropped_spans;
          consumeStreamSpans(stream_buffer_spans_, raw_drop, &dropped_spans);
          accountConsumedSpans(dropped_spans, false, nowNs());
          memmove(
            stream_buffer_.data(),
            stream_buffer_.data() + raw_drop,
            stream_buffer_.size() - raw_drop);
          stream_buffer_.resize(stream_buffer_.size() - raw_drop);
          dropped_bytes_ += raw_drop;
          dropped_events_++;
        }
      }

      gst_buffer_unmap(buffer, &map);
    }

    gst_sample_unref(sample);
  }

  if (!notify_sender && !stream_buffer_.empty() && stream_buffer_first_byte_ns_ > 0 && max_tx_delay_ns > 0) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    const int64_t age_ns = nowNs() - stream_buffer_first_byte_ns_;
    if (!stream_buffer_.empty() && age_ns >= max_tx_delay_ns) {
      const size_t valid_data_bytes = stream_buffer_.size();
      TxPacket packet;
      packet.payload.resize(packet_bytes, 0U);
      memcpy(packet.payload.data(), stream_buffer_.data(), valid_data_bytes);
      packet.payload[packet_bytes - 3] = payload_data_seq_;
      payload_data_seq_ = static_cast<uint8_t>(payload_data_seq_ + 1U);

      const uint16_t pad_zero_count = static_cast<uint16_t>(packet_bytes - valid_data_bytes);
      packet.payload[packet_bytes - 2] = static_cast<uint8_t>(pad_zero_count & 0xFFU);
      packet.payload[packet_bytes - 1] = static_cast<uint8_t>((pad_zero_count >> 8) & 0xFFU);
      consumeStreamSpans(stream_buffer_spans_, valid_data_bytes, &packet.spans);
      tx_queue_.push_back(std::move(packet));
      stream_buffer_.clear();
      stream_buffer_first_byte_ns_ = 0;
      notify_sender = true;
    }
  }

  if (notify_sender) {
    tx_cv_.notify_one();
  }
}

void VideoEncoder::poll_gstreamer_bus()
{
  if (bus_ == nullptr) {
    return;
  }

  bool saw_error = false;
  bool saw_eos = false;

  while (true) {
    GstMessage * msg = gst_bus_pop(bus_);
    if (msg == nullptr) {
      break;
    }

    switch (GST_MESSAGE_TYPE(msg)) {
      case GST_MESSAGE_ERROR: {
        GError * err = nullptr;
        gchar * debug = nullptr;
        gst_message_parse_error(msg, &err, &debug);
        LogError(std::string("Encoder pipeline error: ") +
          (err != nullptr ? err->message : "unknown") +
          (debug != nullptr ? std::string(" debug=") + debug : std::string()));
        if (err != nullptr) {
          g_error_free(err);
        }
        if (debug != nullptr) {
          g_free(debug);
        }
        saw_error = true;
        break;
      }
      case GST_MESSAGE_EOS:
        LogWarn("Encoder pipeline received EOS");
        saw_eos = true;
        break;
      case GST_MESSAGE_WARNING: {
        GError * err = nullptr;
        gchar * debug = nullptr;
        gst_message_parse_warning(msg, &err, &debug);
        LogWarn(std::string("Encoder pipeline warning: ") +
          (err != nullptr ? err->message : "unknown") +
          (debug != nullptr ? std::string(" debug=") + debug : std::string()));
        if (err != nullptr) {
          g_error_free(err);
        }
        if (debug != nullptr) {
          g_free(debug);
        }
        break;
      }
      default:
        break;
    }

    gst_message_unref(msg);
  }

  if (saw_error || saw_eos) {
    restart_gstreamer_pipeline(saw_error ? "bus error" : "bus eos");
  }
}

void VideoEncoder::watchdog_gstreamer_pipeline(const int64_t now_ns)
{
  constexpr int64_t kRecentInputNs = 500000000LL;
  constexpr int64_t kNoOutputStallNs = 2000000000LL;

  if (pipeline_ == nullptr || last_input_frame_ns_ <= 0) {
    return;
  }

  if ((now_ns - last_input_frame_ns_) > kRecentInputNs) {
    return;
  }

  const int64_t last_output_ns = (last_encoded_sample_ns_ > 0) ? last_encoded_sample_ns_ : pipeline_started_ns_;
  if (last_output_ns <= 0 || (now_ns - last_output_ns) < kNoOutputStallNs) {
    return;
  }

  if ((now_ns - last_watchdog_log_ns_) >= 500000000LL) {
    last_watchdog_log_ns_ = now_ns;
    std::ostringstream oss;
    oss << "Encoder pipeline watchdog stall: pushedFrames=" << pushed_frame_count_
        << " pulledSamples=" << pulled_sample_count_
        << " lastOutputAgoMs=" << ((now_ns - last_output_ns) / 1000000LL);
    LogWarn(oss.str());
  }

  restart_gstreamer_pipeline("watchdog stall");
}

void VideoEncoder::maybeLogRuntimeStats(const int64_t now_ns)
{
  if (now_ns - last_runtime_report_ns_ < 1000000000LL) {
    return;
  }

  size_t queue_packets = 0;
  size_t stream_buffer_bytes = 0;
  {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    queue_packets = tx_queue_.size();
    stream_buffer_bytes = stream_buffer_.size();
  }

  const int64_t last_output_ns = (last_encoded_sample_ns_ > 0) ? last_encoded_sample_ns_ : pipeline_started_ns_;
  std::ostringstream oss;
  oss << "Encoder runtime: input=" << input_frame_count_
      << " throttled=" << throttled_frame_count_
      << " pushed=" << pushed_frame_count_
      << " pushFailed=" << push_failed_count_
      << " pulled=" << pulled_sample_count_
      << " sent=" << sent_packet_count_
      << " sendFailed=" << send_failed_count_
      << " queue=" << queue_packets
      << " streamBuffer=" << stream_buffer_bytes << "B";
  if (last_output_ns > 0) {
    oss << " lastOutputAgoMs=" << ((now_ns - last_output_ns) / 1000000LL);
  }
  LogInfo(oss.str());
  last_runtime_report_ns_ = now_ns;
}

void VideoEncoder::restart_gstreamer_pipeline(const char * reason)
{
  LogWarn(std::string("Restarting encoder pipeline: ") + (reason != nullptr ? reason : "unknown"));
  shutdown_gstreamer();
  initialize_gstreamer();

  std::lock_guard<std::mutex> lock(buffer_mutex_);
  pending_input_frames_.clear();
  active_frame_tracks_.clear();
  stream_buffer_.clear();
  stream_buffer_spans_.clear();
  stream_buffer_first_byte_ns_ = 0;
  last_encoded_sample_ns_ = 0;
}

void VideoEncoder::tx_loop()
{
  while (tx_running_) {
    TxPacket packet;

    {
      std::unique_lock<std::mutex> lock(buffer_mutex_);
      tx_cv_.wait(lock, [this]() { return !tx_running_ || !tx_queue_.empty(); });
      if (!tx_running_) {
        return;
      }

      if (next_tx_deadline_ns_ == 0) {
        next_tx_deadline_ns_ = nowNs();
      }

      while (tx_running_) {
        const int64_t now = nowNs();
        if (now >= next_tx_deadline_ns_) {
          break;
        }
        tx_cv_.wait_for(lock, std::chrono::nanoseconds(next_tx_deadline_ns_ - now));
        if (!tx_running_) {
          return;
        }
      }

      if (tx_queue_.empty()) {
        continue;
      }

      packet = std::move(tx_queue_.front());
      tx_queue_.pop_front();

      const int64_t now = nowNs();
      next_tx_deadline_ns_ += frame_interval_ns_;
      if (next_tx_deadline_ns_ < now - frame_interval_ns_) {
        next_tx_deadline_ns_ = now + frame_interval_ns_;
      }
    }

    bool sent_ok = false;
    if (serial_sender_ && serial_sender_->isOpen()) {
      size_t written = 0;
      sent_ok = serial_sender_->sendOnce(packet.payload, &written);
      (void)written;
    } else if (mqtt_sender_ && mqtt_sender_->isOpen()) {
      sent_ok = mqtt_sender_->sendPayload(packet.payload);
    }

    if (!sent_ok) {
      ++send_failed_count_;
      LogWarn("Timed transport send failed");
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }

    ++sent_packet_count_;
    packet_sequence_id_++;
    const int64_t completion_ns = nowNs();
    {
      std::lock_guard<std::mutex> lock(buffer_mutex_);
      accountConsumedSpans(packet.spans, true, completion_ns);
      maybeLogLatencyStats(completion_ns);
    }


    const int64_t telemetry_ns = nowNs();
    if (telemetry_ns - last_telemetry_ns_ > 1000000000LL) {
      size_t queue_packets = 0;
      uint64_t dropped_bytes = 0;
      {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        queue_packets = tx_queue_.size();
        dropped_bytes = dropped_bytes_;
      }

      std::ostringstream oss;
      oss << "TX timer stats: queue=" << queue_packets
          << "pkts dropped=" << dropped_bytes << "B";
      {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        oss << " streamBuffer=" << stream_buffer_.size() << "B";
      }
      if (serial_sender_ && serial_sender_->isOpen()) {
        oss << " seq=" << static_cast<unsigned>(serial_sender_->sequence());
      }
      LogInfo(oss.str());
      last_telemetry_ns_ = telemetry_ns;
    }
  }
}

void VideoEncoder::appendStreamSpan(std::deque<ByteSpan> & spans, const uint64_t track_id, const size_t bytes)
{
  if (bytes == 0U) {
    return;
  }

  if (!spans.empty() && spans.back().track_id == track_id) {
    spans.back().bytes += bytes;
    return;
  }

  spans.push_back(ByteSpan{track_id, bytes});
}

void VideoEncoder::consumeStreamSpans(
  std::deque<ByteSpan> & spans,
  size_t bytes_to_consume,
  std::vector<ByteSpan> * consumed_spans)
{
  while (bytes_to_consume > 0U && !spans.empty()) {
    ByteSpan & front = spans.front();
    const size_t take = std::min(front.bytes, bytes_to_consume);

    if (consumed_spans != nullptr) {
      if (!consumed_spans->empty() && consumed_spans->back().track_id == front.track_id) {
        consumed_spans->back().bytes += take;
      } else {
        consumed_spans->push_back(ByteSpan{front.track_id, take});
      }
    }

    front.bytes -= take;
    bytes_to_consume -= take;
    if (front.bytes == 0U) {
      spans.pop_front();
    }
  }
}

void VideoEncoder::accountConsumedSpans(
  const std::vector<ByteSpan> & spans,
  const bool sent,
  const int64_t completion_ns)
{
  for (const ByteSpan & span : spans) {
    const auto track_it = active_frame_tracks_.find(span.track_id);
    if (track_it == active_frame_tracks_.end()) {
      continue;
    }

    FrameTrack & track = track_it->second;
    if (sent) {
      track.sent_bytes += span.bytes;
    } else {
      track.dropped_bytes += span.bytes;
    }

    if ((track.sent_bytes + track.dropped_bytes) < track.encoded_bytes) {
      continue;
    }

    if (track.dropped_bytes == 0U && track.input_time_ns > 0) {
      const double latency_ms =
        static_cast<double>(completion_ns - track.input_time_ns) / 1000000.0;
      latency_latest_ms_ = latency_ms;
      latency_latest_frame_id_ = track.source_frame_id;
      latency_sum_ms_ += latency_ms;
      latency_max_ms_ = std::max(latency_max_ms_, latency_ms);
      if (latency_min_ms_ < 0.0 || latency_ms < latency_min_ms_) {
        latency_min_ms_ = latency_ms;
      }
      latency_sample_count_++;
    } else if (track.dropped_bytes > 0U) {
      std::ostringstream oss;
      oss << "Frame " << track.source_frame_id << " dropped before full send: sent="
          << track.sent_bytes << "B dropped=" << track.dropped_bytes
          << "B encoded=" << track.encoded_bytes << "B";
      LogWarn(oss.str());
    }

    retireFrameTrackIfDone(span.track_id);
  }
}

void VideoEncoder::maybeLogLatencyStats(const int64_t now_ns)
{
  if (latency_sample_count_ == 0U) {
    return;
  }

  if (last_latency_report_ns_ != 0 && (now_ns - last_latency_report_ns_) < 1000000000LL) {
    return;
  }

  // std::ostringstream oss;
  // oss << std::fixed << std::setprecision(2)
  //     << "Capture->last-packet latency: last=" << latency_latest_ms_
  //     << "ms frame=" << latency_latest_frame_id_
  //     << " avg=" << (latency_sum_ms_ / static_cast<double>(latency_sample_count_))
  //     << "ms min=" << latency_min_ms_
  //     << "ms max=" << latency_max_ms_
  //     << "ms samples=" << latency_sample_count_;
  // LogInfo(oss.str());
  last_latency_report_ns_ = now_ns;
}

void VideoEncoder::retireFrameTrackIfDone(const uint64_t track_id)
{
  const auto track_it = active_frame_tracks_.find(track_id);
  if (track_it == active_frame_tracks_.end()) {
    return;
  }

  const FrameTrack & track = track_it->second;
  if ((track.sent_bytes + track.dropped_bytes) >= track.encoded_bytes) {
    active_frame_tracks_.erase(track_it);
  }
}

void VideoEncoder::display_loop()
{
  cv::namedWindow("Doorlock Sniper Raw", cv::WINDOW_NORMAL);
  cv::namedWindow("Doorlock Sniper ROI", cv::WINDOW_NORMAL);
  cv::namedWindow("Doorlock Sniper Static", cv::WINDOW_NORMAL);
  cv::namedWindow("Doorlock Sniper", cv::WINDOW_NORMAL);
  cv::setWindowProperty("Doorlock Sniper Raw", cv::WND_PROP_ASPECT_RATIO, cv::WINDOW_KEEPRATIO);
  cv::resizeWindow("Doorlock Sniper ROI", options_.output_size, options_.output_size);
  cv::resizeWindow("Doorlock Sniper Static", options_.output_size, options_.output_size);
  cv::resizeWindow("Doorlock Sniper", options_.output_size, options_.output_size);

  while (display_running_) {
    cv::Mat raw_frame;
    cv::Mat roi_frame;
    cv::Mat static_frame;
    cv::Mat frame;

    {
      std::lock_guard<std::mutex> lock(frame_mutex_);
      if (!display_raw_frame_.empty()) display_raw_frame_.copyTo(raw_frame);
      if (!display_roi_frame_.empty()) display_roi_frame_.copyTo(roi_frame);
      if (!display_static_frame_.empty()) display_static_frame_.copyTo(static_frame);
      if (!display_frame_.empty()) display_frame_.copyTo(frame);
    }

    if (!raw_frame.empty()) cv::imshow("Doorlock Sniper Raw", raw_frame);
    if (!roi_frame.empty()) cv::imshow("Doorlock Sniper ROI", roi_frame);
    if (!static_frame.empty()) cv::imshow("Doorlock Sniper Static", static_frame);
    if (!frame.empty()) cv::imshow("Doorlock Sniper", frame);

    if (options_.debug_dump_enable && !frame.empty()) {
      display_frame_counter_++;
      if ((display_frame_counter_ % static_cast<uint64_t>(options_.debug_dump_every_n_frames)) == 0U) {
        const std::filesystem::path dump_dir = std::filesystem::path(options_.debug_dump_dir) / "encoder";
        std::ostringstream idx;
        idx << std::setw(8) << std::setfill('0') << display_frame_counter_;
        const std::string frame_id = idx.str();

        if (options_.debug_dump_save_raw && !raw_frame.empty()) {
          cv::imwrite((dump_dir / ("raw_" + frame_id + ".png")).string(), raw_frame);
        }
        if (options_.debug_dump_save_roi && !roi_frame.empty()) {
          cv::imwrite((dump_dir / ("roi_" + frame_id + ".png")).string(), roi_frame);
        }
        if (options_.debug_dump_save_static && !static_frame.empty()) {
          cv::imwrite((dump_dir / ("static_" + frame_id + ".png")).string(), static_frame);
        }
        if (options_.debug_dump_save_final) {
          cv::imwrite((dump_dir / ("final_" + frame_id + ".png")).string(), frame);
        }
      }
    }

    cv::waitKey(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }

  cv::destroyWindow("Doorlock Sniper Raw");
  cv::destroyWindow("Doorlock Sniper ROI");
  cv::destroyWindow("Doorlock Sniper Static");
  cv::destroyWindow("Doorlock Sniper");
}

}  // namespace doorlock_sniper
