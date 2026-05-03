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

constexpr size_t kVideoPacketBytes = 300U; // 每个视频数据包的总字节数，必须固定以便接收端正确解析
constexpr size_t kPayloadHeaderBytes = 8U; // 每个视频数据包中预留的帧头字节数，用于存储帧相关信息，如跟踪ID和帧内序号，必须固定以便接收端正确解析
constexpr size_t kVideoDataBytes = kVideoPacketBytes - kPayloadHeaderBytes; // 每个视频数据包中实际的视频数据字节数，必须固定以便接收端正确解析

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

void WriteLe16(std::vector<uint8_t> & payload, const size_t offset, const uint16_t value)
{
  payload[offset] = static_cast<uint8_t>(value & 0xFFU);
  payload[offset + 1U] = static_cast<uint8_t>((value >> 8) & 0xFFU);
}

void WriteLe32(std::vector<uint8_t> & payload, const size_t offset, const uint32_t value)
{
  payload[offset] = static_cast<uint8_t>(value & 0xFFU);
  payload[offset + 1U] = static_cast<uint8_t>((value >> 8) & 0xFFU);
  payload[offset + 2U] = static_cast<uint8_t>((value >> 16) & 0xFFU);
  payload[offset + 3U] = static_cast<uint8_t>((value >> 24) & 0xFFU);
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
  constexpr int kFixedOutputFps = 50;

  if (options_.output_fps != kFixedOutputFps) {
    LogWarn("output_fps forced to fixed 50Hz");
  }
  options_.output_fps = kFixedOutputFps;
  if (options_.packet_size != kVideoPacketBytes) {
    LogWarn("packet_size overridden to fixed 300 bytes");
    options_.packet_size = kVideoPacketBytes;
  }
  if (static_cast<size_t>(options_.packet_size) != kVideoPacketBytes) {
    throw std::runtime_error("Invalid packet size, payload must be fixed 300 bytes");
  }
  if (kPayloadHeaderBytes >= static_cast<size_t>(options_.packet_size)) {
    throw std::runtime_error("Invalid packet split, 300-byte payload must reserve 8-byte frame header");
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
    "stream-type", 0,
    "format", GST_FORMAT_TIME,
    "is-live", TRUE,
    "do-timestamp", TRUE,
    nullptr);
  gst_caps_unref(caps);

  const bool low_bitrate_mode = (options_.target_bitrate <= 80);
  const int key_int = std::max(options_.output_fps, 40); // 关键帧间隔设置为1秒或更短，避免过长的关键帧间隔导致解码器在丢包或错误恢复时需要等待过久，尤其在极低比特率下更容易出现质量崩溃和解码失败的情况
  // const int key_int = 40; // 固定关键帧间隔为40帧，约0.8秒，避免过长的关键帧间隔导致解码器在丢包或错误恢复时需要等待过久
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
      "bitrate", options_.target_bitrate, // x264enc的bitrate参数单位是kbps
      "speed-preset", speed_preset, // 预设级别，数值越大编码越慢但质量越好，0是默认，1-10分别对应ultrafast到placebo
      "tune", 0x00000004, // 不使用特殊调优，保持默认设置，x264enc的tune参数可以设置为zerolatency等以优化特定场景，但在极低比特率下可能不适用
      "byte-stream", TRUE, // 输出Annex B格式的H.264字节流，适合网络传输
      "key-int-max", key_int, // 最大关键帧间隔，单位是帧数，设置为max(output_fps, 40)，避免过长的关键帧间隔导致解码器在丢包或错误恢复时需要等待过久
      "bframes", 2, // B帧数量，设置为4可以在低比特率下提高压缩效率，但过多的B帧可能增加编码延迟和复杂度
      "rc-lookahead", 0, // 码率控制的前瞻帧数，设置为40可以让编码器更好地分析未来的帧以优化码率分配，但过多的前瞻可能增加编码延迟
      "sync-lookahead", 0, // 同步前瞻帧数，设置为20可以在保持较低延迟的同时提供一定的前瞻分析能力，过多可能增加延迟
      "sliced-threads", FALSE, // 禁用切片线程，切片线程在低比特率和小分辨率下可能反而增加编码复杂度和降低效率
      "ref", 1, // 参考帧数量，设置为5可以在低比特率下提高压缩效率，但过多的参考帧可能增加编码复杂度和内存使用
      "aud", TRUE, // 在每个访问单元前插入访问单元分界符，适合网络传输
      "vbv-buf-capacity", 500, // VBV缓冲区容量，单位是kbps，设置为500可以在低比特率下提供足够的缓冲以避免码率过高时的质量崩溃，但过大可能增加编码延迟

      "option-string", // 其他x264编码选项，通过option-string参数传递，具体选项可以参考x264的文档和源代码，以下是一些可能有助于低比特率编码的选项：
      // repeat-headers=1:每个关键帧前重复SPS/PPS等参数集，增加丢包恢复能力但增加码流大小
      // scenecut=0:禁用场景切割，保持稳定的关键帧间隔，适合实时视频流
      // ref=1:使用单参考帧，减少编码复杂度但可能降低质量
      // aq-mode=2:启用自适应量化，增强低比特率下的视觉质量
      // aq-strength=1.2:自适应量化强度，数值
      // mbtree=1:启用宏块树优化，改善低比特率编码效率
      // qcomp=0.75:量化参数压缩率，较高的数值在低比特率下可能提供更好的质量但增加码流大小
      // subme=8:亚像素运动估计精度，设置为8提供最高精度但增加编码时间，低比特率下可能有助于提高质量
      // trellis=2:启用RDO量化，设置为2提供最佳质量但增加编码时间，低比特率下可能有助于提高质量
      // deblock=1,1:启用去块滤波，设置为1,1提供适度的去块效果，改善低比特率下的视觉质量，但过强可能增加编码复杂度
      // force-cfr=1:强制恒定帧率，适合实时视频流
      "repeat-headers=1:scenecut=0:aq-mode=2:aq-strength=1.2:mbtree=1:qcomp=0.75:" 
      "subme=8:trellis=2:deblock=1,1:force-cfr=1",
      "pass", 0, // 单遍模式，适合实时编码，x264enc的pass参数可以设置为1或2以启用两遍编码以提高质量，但会增加编码时间，不适合实时视频流
      nullptr);
  } else {
    g_object_set(
      G_OBJECT(encoder),
      "bitrate", options_.target_bitrate,
      "speed-preset", speed_preset,
      "tune", 0x00000004,
      "byte-stream", TRUE,
      "key-int-max", 25,
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
    "config-interval", -1, // 不周期性插入SPS/PPS等参数集，交由encoder的repeat-headers选项控制，避免不必要的码流膨胀
    "disable-passthrough", TRUE, // 禁止parser直接传递输入数据，确保所有数据都经过parser处理以正确生成访问单元分界符等必要的码流结构，适合网络传输
    nullptr);

  GstCaps * h264_caps = gst_caps_new_simple(
    "video/x-h264", // 指定输出H.264编码格式，适合网络传输
    "stream-format", G_TYPE_STRING, "byte-stream", // 输出Annex B格式的H.264字节流，适合网络传输
    "alignment", G_TYPE_STRING, "au", // 以访问单元为对齐方式，确保每个样本包含完整的访问单元，适合网络传输
    nullptr);

  g_object_set(
    G_OBJECT(appsink_),
    "caps", h264_caps, // 设置appsink的caps以匹配parser的输出，确保数据格式正确，适合网络传输
    "max-buffers", 2, // 限制appsink内部队列长度为2，避免过多的编码帧积压在内存中增加延迟
    "drop", TRUE, // 当appsink队列满时丢弃新帧，保持最新的内容，适合实时视频流
    "emit-signals", FALSE, // 禁用appsink的信号机制，改为使用gst_app_sink_try_pull_sample非阻塞拉取样本，适合实时视频流
    "sync", FALSE, // 禁用appsink的同步机制，允许在不同线程中拉取样本，适合实时视频流
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

cv::Mat VideoEncoder::preprocess_image(const cv::Mat & input,cv::Mat * roi_downsample,cv::Mat * static_removed)
{
  int x = (input.cols - options_.crop_size) / 2; // 计算水平裁剪起点：从图像中心开始，使裁剪区域居中
  int y = 0; // 垂直裁剪起点设为0，仅在水平方向上居中
  x = std::max(0, x); // 确保x坐标不为负数，防止越界访问
  y = std::max(0, y); // 确保y坐标不为负数
  const int w = std::min(options_.crop_size, input.cols - x); // 计算实际裁剪宽度，确保不超过剩余的图像宽度
  const int h = std::min(options_.crop_size, input.rows - y); // 计算实际裁剪高度，确保不超过剩余的图像高度

  cv::Mat cropped = input(cv::Rect(x, y, w, h)); // 从输入图像中提取感兴趣区域（ROI）
  cv::Mat resized;
  cv::resize(cropped, resized, cv::Size(options_.output_size, options_.output_size), 0, 0, cv::INTER_LINEAR); // 调整图像大小到输出分辨率

  if (roi_downsample) { // 如果提供了roi_downsample指针，将缩放后的图像复制到该指针指向的矩阵
    resized.copyTo(*roi_downsample);
  }

  cv::Mat working = resized; // 将缩放后的图像作为工作图像
  if (options_.force_monochrome) { // 如果启用强制单色模式，将图像转换为灰度后再转回BGR
    cv::Mat gray_full;
    cv::cvtColor(working, gray_full, cv::COLOR_BGR2GRAY); // 转换为灰度图
    cv::cvtColor(gray_full, working, cv::COLOR_GRAY2BGR); // 将灰度图转换回BGR格式（每个通道相同）
  }

  if (!options_.static_simplify) { // 如果未启用静态背景移除，直接返回处理后的图像
    if (static_removed) { // 如果提供了static_removed指针，将当前工作图像复制到该指针指向的矩阵
      working.copyTo(*static_removed);
    }
    return working;
  }

  cv::Mat gray; // 将工作图像转换为灰度用于背景检测
  cv::cvtColor(working, gray, cv::COLOR_BGR2GRAY);
  if (background_gray_f32_.empty()) { // 如果这是第一帧，初始化背景图像
    gray.convertTo(background_gray_f32_, CV_32F); // 将灰度图转换为浮点格式存储为背景
    return working;
  }

  cv::Mat bg_u8; // 将浮点背景转换回8位格式用于差异计算
  cv::convertScaleAbs(background_gray_f32_, bg_u8);

  cv::Mat diff; // 计算当前帧与背景之间的绝对差异
  cv::absdiff(gray, bg_u8, diff);

  cv::Mat motion_mask; // 使用阈值将差异转换为二值运动掩模
  cv::threshold(diff, motion_mask, options_.motion_threshold, 255, cv::THRESH_BINARY);

  if (options_.motion_erode_px > 0) { // 对运动掩模进行腐蚀操作来移除小的噪声
    if (motion_erode_kernel_.empty()) { // 如果腐蚀核尚未创建，创建一个椭圆形腐蚀核
      const int k = 2 * options_.motion_erode_px + 1;
      motion_erode_kernel_ = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(k, k));
    }
    cv::erode(motion_mask, motion_mask, motion_erode_kernel_, cv::Point(-1, -1), 1); // 执行腐蚀操作
  }

  if (options_.motion_dilate_px > 0) { // 对运动掩模进行膨胀操作来填充小的间隙
    if (motion_dilate_kernel_.empty()) { // 如果膨胀核尚未创建，创建一个椭圆形膨胀核
      const int k = 2 * options_.motion_dilate_px + 1;
      motion_dilate_kernel_ = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(k, k));
    }
    cv::dilate(motion_mask, motion_mask, motion_dilate_kernel_, cv::Point(-1, -1), 1); // 执行膨胀操作
  }

  const double motion_ratio_raw = // 计算运动掩模中非零像素的比例
    static_cast<double>(cv::countNonZero(motion_mask)) / static_cast<double>(motion_mask.total());
  const bool suppress_trail = (motion_ratio_raw >= options_.trail_disable_motion_ratio); // 判断是否运动比例过高，需要禁用运动拖尾效果

  if (options_.center_clear_size > 0) { // 如果启用中心清除，在中心区域填充运动掩模为白色（强制显示）
    const int clear_size = std::min({options_.center_clear_size, working.cols, working.rows}); // 计算清除区域的实际大小
    const int x0 = std::max(0, working.cols / 2 - clear_size / 2); // 计算清除区域的起始坐标（图像中心）
    const int y0 = std::max(0, working.rows / 2 - clear_size / 2); // 计算清除区域的起始坐标（图像中心）
    const int cw = std::min(clear_size, working.cols - x0); // 计算清除区域的实际宽度和高度
    const int ch = std::min(clear_size, working.rows - y0); // 计算清除区域的实际高度
    cv::rectangle(motion_mask, cv::Rect(x0, y0, cw, ch), cv::Scalar(255), cv::FILLED); // 在中心区域绘制白色矩形
  }

  cv::Mat static_base = working.clone(); // 克隆工作图像作为静态背景基础
  if (!options_.force_monochrome && options_.target_bitrate <= 80) { // 如果未启用强制单色且目标比特率较低，转换静态背景为单色
    cv::Mat gray_bg;
    cv::cvtColor(static_base, gray_bg, cv::COLOR_BGR2GRAY); // 转换为灰度图
    cv::cvtColor(gray_bg, static_base, cv::COLOR_GRAY2BGR); // 转换回BGR格式
  }

  cv::Mat blurred_static; // 对静态背景进行高斯模糊操作
  cv::GaussianBlur(
    static_base,
    blurred_static,
    cv::Size(),
    std::max(0.0, options_.bg_blur_sigma),
    std::max(0.0, options_.bg_blur_sigma));

  cv::Mat focused = blurred_static.clone(); // 创建焦点图像，初始为模糊的静态背景
  working.copyTo(focused, motion_mask); // 使用运动掩模将原始工作图像复制到焦点图像（只复制运动区域）
  if (static_removed) { // 如果提供了static_removed指针，将焦点图像复制到该指针指向的矩阵
    focused.copyTo(*static_removed);
  }

  if (options_.motion_trail_frames > 0) { // 如果启用运动拖尾效果
    motion_mask_history_.push_back(motion_mask.clone()); // 将当前帧的运动掩模添加到历史记录
    trail_frame_history_.push_back(working.clone()); // 将当前帧添加到历史记录
    const size_t max_history = static_cast<size_t>(options_.motion_trail_frames + 1); // 计算允许的最大历史记录大小
    while (motion_mask_history_.size() > max_history) { // 移除超出大小限制的旧运动掩模
      motion_mask_history_.pop_front();
    }
    while (trail_frame_history_.size() > max_history) { // 移除超出大小限制的旧帧
      trail_frame_history_.pop_front();
    }

    const size_t history_size = motion_mask_history_.size(); // 获取当前历史记录大小
    if (!suppress_trail && history_size > 1 && history_size == trail_frame_history_.size()) { // 如果条件允许，将历史运动拖尾合并到焦点图像中
      cv::Mat trail_mask = motion_mask.clone(); // 初始化拖尾掩模为当前运动掩模
      cv::Mat trail_img = working.clone(); // 初始化拖尾图像为当前工作图像
      for (size_t i = 0; i < history_size - 1; ++i) { // 遍历所有历史帧
        cv::bitwise_or(trail_mask, motion_mask_history_[i], trail_mask); // 将历史运动掩模与拖尾掩模进行按位或操作
        cv::max(trail_img, trail_frame_history_[i], trail_img); // 将历史帧与拖尾图像进行逐像素最大值操作
      }
      trail_img.copyTo(focused, trail_mask); // 使用拖尾掩模将拖尾图像复制到焦点图像中
    }
  } else { // 如果禁用拖尾效果
    motion_mask_history_.clear();
    trail_frame_history_.clear();
  }

  cv::accumulateWeighted(gray, background_gray_f32_, std::clamp(options_.bg_update_alpha, 0.001, 0.2)); // 使用加权累积更新背景灰度图像
  return focused; // 返回处理后的焦点图像
}

bool VideoEncoder::processImage(const cv::Mat & input, const int64_t timestamp_ns, const uint64_t source_frame_id)
{
  if (input.empty()) { // 检查输入图像是否为空，如果为空返回false
    return false;
  }

  ++input_frame_count_; // 增加输入帧计数
  int64_t now = (timestamp_ns > 0) ? timestamp_ns : nowNs(); // 使用提供的时间戳，如果没有则使用当前时间
  if (last_encode_stamp_ns_ > 0 && now <= last_encode_stamp_ns_) { // 如果有上一次编码的时间戳，且当前时间不晚于上一次，则重新获取当前时间以保证时间戳单调递增
    now = nowNs();
  }
  if (last_encode_stamp_ns_ > 0 && (now - last_encode_stamp_ns_) < frame_interval_ns_) { // 检查帧间隔是否足够，如果不足则跳过此帧（进行帧率控制）
    ++throttled_frame_count_; // 增加被限制的帧计数
    LogRuntimeStats(nowNs()); // 记录运行时统计信息
    return true; // 虽然跳过编码但返回true表示接受了此帧
  }
  last_encode_stamp_ns_ = now; // 更新最后编码时间戳

  cv::Mat roi_downsample; // 准备用于存储ROI缩小图像的矩阵
  cv::Mat static_removed; // 准备用于存储移除静态背景后图像的矩阵
  cv::Mat processed = preprocess_image(input, &roi_downsample, &static_removed); // 对输入图像进行预处理，包括裁剪、缩放、单色转换、背景去除等

  if (options_.enable_display) { // 如果启用显示模式，生成预览图像
    cv::Mat raw_preview; // 将原始输入图像缩小为一半尺寸用于预览
    cv::resize(
      input,
      raw_preview,
      cv::Size(std::max(1, input.cols / 2), std::max(1, input.rows / 2)),
      0,
      0,
      cv::INTER_AREA);

    std::lock_guard<std::mutex> lock(frame_mutex_); // 获取帧显示互斥锁
    raw_preview.copyTo(display_raw_frame_); // 将原始预览图像复制到显示缓冲区
    roi_downsample.copyTo(display_roi_frame_); // 将ROI缩小图像复制到显示缓冲区
    static_removed.copyTo(display_static_frame_); // 将移除静态背景的图像复制到显示缓冲区
    processed.copyTo(display_frame_); // 将处理后的最终图像复制到显示缓冲区
  }

  const uint64_t frame_token = reserveFrameToken(now, source_frame_id); // 为此帧预留一个唯一的帧令牌用于追踪
  push_frame_to_gstreamer(processed, frame_token); // 将处理后的帧推送到GStreamer管道
  pull_stream_and_packetize(); // 从GStreamer拉取已编码的数据流并进行打包
  poll_gstreamer_bus(); // 处理GStreamer总线上的消息和事件
  LogRuntimeStats(nowNs()); // 记录运行时统计信息
  frame_count_++; // 增加处理的帧计数
  return true; // 返回true表示帧已成功接受并进行处理
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
      const GstClockTime pts =
        static_cast<GstClockTime>((frame_token - 1U) * static_cast<uint64_t>(frame_interval_ns_));
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

  const size_t packet_bytes = static_cast<size_t>(options_.packet_size);
  const size_t max_backlog_bytes = static_cast<size_t>(
    options_.bandwidth_limit_kbytes * 1000.0 * options_.max_tx_delay_s);
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
      track.payload_frame_id = next_payload_frame_id_++;
      track.input_time_ns = pending_found ? pending_input.input_time_ns : fallback_input_ns;
      track.enqueue_time_ns = pending_found ? pending_input.enqueue_time_ns : fallback_input_ns;
      track.encoded_bytes = map.size;
      active_frame_tracks_[track_id] = track;

      const uint32_t frame_total_bytes = static_cast<uint32_t>(
        std::min<size_t>(map.size, static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
      const size_t fragment_count_size_t =
        (map.size + kVideoDataBytes - 1U) / kVideoDataBytes;
      const uint16_t fragment_count = static_cast<uint16_t>(
        std::min<size_t>(fragment_count_size_t, static_cast<size_t>(std::numeric_limits<uint16_t>::max())));
      uint16_t fragment_index = 0;
      size_t offset = 0U;
      while (offset < map.size) {
        const size_t valid_data_bytes = std::min(kVideoDataBytes, map.size - offset);
        TxPacket packet;
        packet.payload.resize(packet_bytes, 0U);
        packet.source_frame_id = track.source_frame_id;
        packet.payload_frame_id = track.payload_frame_id;
        packet.fragment_index = fragment_index;
        packet.fragment_count = fragment_count;
        WriteLe16(packet.payload, 0U, track.payload_frame_id);
        WriteLe16(packet.payload, 2U, fragment_index);
        WriteLe32(packet.payload, 4U, frame_total_bytes);
        memcpy(packet.payload.data() + kPayloadHeaderBytes, map.data + offset, valid_data_bytes);
        packet.spans.push_back(ByteSpan{track_id, valid_data_bytes});
        tx_queue_.push_back(std::move(packet));
        notify_sender = true;
        offset += valid_data_bytes;
        fragment_index = static_cast<uint16_t>(fragment_index + 1U);
      }

      const size_t queued_bytes = tx_queue_.size() * packet_bytes;
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
      }

      gst_buffer_unmap(buffer, &map);
    }

    gst_sample_unref(sample);
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

void VideoEncoder::LogRuntimeStats(const int64_t now_ns)
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
  // std::ostringstream oss;
  // oss << "Encoder runtime: input=" << input_frame_count_
  //     << " throttled=" << throttled_frame_count_
  //     << " pushed=" << pushed_frame_count_
  //     << " pushFailed=" << push_failed_count_
  //     << " pulled=" << pulled_sample_count_
  //     << " sent=" << sent_packet_count_
  //     << " sendFailed=" << send_failed_count_
  //     << " queue=" << queue_packets
  //     << " streamBuffer=" << stream_buffer_bytes << "B";
  // if (last_output_ns > 0) {
  //   oss << " lastOutputAgoMs=" << ((now_ns - last_output_ns) / 1000000LL);
  // }
  // LogInfo(oss.str());
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
    // if (static_cast<int>(packet.payload_frame_id) != last_logged_tx_payload_frame_id_) {
    //   std::ostringstream oss;
    //   oss << "TX sending frame source=" << packet.source_frame_id
    //       << " payloadFrame=" << packet.payload_frame_id
    //       << " fragments=" << packet.fragment_count;
    //   if (packet.fragment_count > 0U) {
    //     oss << " firstFragment=" << (static_cast<unsigned>(packet.fragment_index) + 1U)
    //         << "/" << packet.fragment_count;
    //   }
    //   LogInfo(oss.str());
    //   last_logged_tx_payload_frame_id_ = static_cast<int>(packet.payload_frame_id);
    // }
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
      LogLatencyStats(completion_ns);
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

void VideoEncoder::LogLatencyStats(const int64_t now_ns)
{
  if (latency_sample_count_ == 0U) {
    return;
  }

  if (last_latency_report_ns_ != 0 && (now_ns - last_latency_report_ns_) < 1000000000LL) {
    return;
  }

// 估计从捕获到发送完成的延迟统计，单位为毫秒，包含最新的单帧延迟、平均延迟、最小延迟、最大延迟以及样本数量等信息，以便监控编码性能和网络传输状况 
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
