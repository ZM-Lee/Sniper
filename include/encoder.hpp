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
    std::string send_chain = "serial"; // serial or mqtt
    int crop_size = 800; // 设置为0以禁用裁剪
    int output_size = 400; // 输出视频的宽高，必须是偶数
    int output_fps = 50;  // 输出视频帧率
    int target_bitrate = 40; // 目标视频比特率，单位kbps
    int packet_size = 300; // 每个数据包的字节数，必须小于等于300
    bool static_simplify = true; // 启用静态区域简化以减少编码复杂度
    int motion_threshold = 14; // 像素差异阈值，低于此值的像素将被视为静态
    int motion_erode_px = 1; // 运动掩码腐蚀半径（像素），有助于去除噪点
    int motion_dilate_px = 2; // 运动掩码膨胀半径（像素），有助于连接断开的运动区域
    int motion_trail_frames = 3; // 运动拖尾帧数，在检测到运动时保留前几帧的运动区域以形成拖尾效果
    double trail_disable_motion_ratio = 0.30; // 当运动区域占比超过此值时禁用拖尾效果，避免过多运动时拖尾反而增加编码复杂度
    double bg_update_alpha = 0.01; // 背景更新的学习率，较小的值会使背景更新更慢但更稳定
    double bg_blur_sigma = 1.2; // 背景模糊的高斯核标准差，有助于减少细节并降低编码复杂度
    int center_clear_size = 100; // 中心清除区域大小（像素），在门锁场景中可以去除中心区域的运动检测以减少误报
    bool force_monochrome = true; // 强制输出单色视频，即使输入是彩色的，这在极低比特率下可能有助于降低编码复杂度
    double bandwidth_limit_kbytes = 14.9; // 发送带宽限制，单位KB/s，用于动态调整发送速率以避免网络拥塞
    double bandwidth_window_s = 0.5; // 计算发送速率的时间窗口，单位秒，较大的窗口可以更平滑地响应带宽变化但反应更慢
    double max_tx_delay_s = 0.09; // 发送最大延迟，单位秒，如果数据包在队列中等待超过此时间将被丢弃以避免过时的内容占用带宽
    bool enable_display = true; // 启用显示功能
    bool debug_dump_enable = false; // 启用调试转储功能
    int debug_dump_every_n_frames = 20; // 每隔多少帧保存一次调试图像
    bool debug_dump_save_raw = true; // 保存原始图像
    bool debug_dump_save_roi = true; // 保存裁剪后的ROI图像
    bool debug_dump_save_static = true;   // 保存静态区域简化后的图像
    bool debug_dump_save_final = true;  // 保存最终编码前的图像

    std::string serial_port = "/dev/ttyUSB0"; // 串口设备路径
    int serial_baudrate = 921600; // 串口波特率
    int serial_cmd_id = 0x0310; // 串口命令ID，发送时会被封装在数据包头部以便接收端识别

    std::string mqtt_host = "192.168.50.78"; // MQTT服务器地址
    int mqtt_port = 3333; // MQTT服务器端口
    std::string mqtt_topic = "CustomByteBlock"; // MQTT主题
    std::string mqtt_client_id = "doorlock_sniper"; // MQTT客户端ID
    int mqtt_qos = 1; // MQTT服务质量

    std::string x264_preset = "auto"; // x264编码预设，auto表示根据目标比特率自动选择，常见值还有"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow"
    std::string debug_dump_dir = "sniper_debug_imgs"; // 调试图像保存目录，启用调试转储功能时使用
  };

  explicit VideoEncoder(Options options);
  ~VideoEncoder();

  bool processImage(const cv::Mat & input, int64_t timestamp_ns = 0, uint64_t source_frame_id = 0); // 处理输入图像并将其编码发送，返回是否成功接受该帧进行处理

private:
  struct PendingInputFrame
  {
    uint64_t source_frame_id = 0;   // 源帧ID
    int64_t input_time_ns = 0;      // 输入时间戳（纳秒）
    int64_t enqueue_time_ns = 0;    // 入队时间戳（纳秒）
  };

  struct ByteSpan
  {
    uint64_t track_id = 0;  // 跟踪ID
    size_t bytes = 0;       // 字节数
  };

  struct TxPacket
  {
    std::vector<uint8_t> payload; // 数据包负载，包含编码后的视频数据和必要的头部信息
    std::vector<ByteSpan> spans;  // 该数据包对应的ByteSpan列表，用于跟踪发送和确认
    uint64_t source_frame_id = 0; // 源帧ID，便于发送线程实时打印当前正在发送的帧
    uint16_t payload_frame_id = 0; // 编码后的负载帧ID，用于区分发送队列中的不同视频帧
    uint16_t fragment_index = 0; // 当前数据包在所属视频帧中的分片序号
    uint16_t fragment_count = 0; // 当前视频帧总共被切分成多少个数据包
  };

  struct FrameTrack
  {
    uint64_t track_id = 0; // 跟踪ID，唯一标识一个输入帧的编码和发送过程
    uint64_t source_frame_id = 0; // 源帧ID，来自processImage的参数，用于关联输入帧和编码跟踪
    uint16_t payload_frame_id = 0; // 负载帧ID，编码后的视频帧ID，用于发送时的标识和接收端的重组
    int64_t input_time_ns = 0; // 输入时间戳（纳秒），来自processImage的参数，用于计算端到端延迟
    int64_t enqueue_time_ns = 0; // 入队时间戳（纳秒），记录该帧被加入待发送队列的时间，用于计算在队列中的等待时间
    size_t encoded_bytes = 0; // 编码后的字节数，记录该帧编码后生成的数据大小，用于跟踪发送进度和计算带宽占用
    size_t sent_bytes = 0; // 已发送字节数，记录该帧已经成功发送的字节数，用于跟踪发送进度和计算丢包情况
    size_t dropped_bytes = 0; // 已丢弃字节数，记录该帧由于过时或带宽限制等原因被丢弃的字节数，用于统计丢包情况和调整发送策略
  };

  static int64_t nowNs();

  uint64_t reserveFrameToken(int64_t input_time_ns, uint64_t source_frame_id); // 为输入帧保留一个唯一的帧令牌，并记录相关信息以便后续关联编码和发送过程
  void initialize_gstreamer(); // 初始化GStreamer管道
  void shutdown_gstreamer(); // 关闭GStreamer管道并释放资源

  cv::Mat preprocess_image(
    const cv::Mat & input,
    cv::Mat * roi_downsample = nullptr,
    cv::Mat * static_removed = nullptr);

  void push_frame_to_gstreamer(const cv::Mat & frame, uint64_t frame_token); // 将预处理后的图像推送到GStreamer管道进行编码，frame_token用于关联输入帧和编码过程
  void pull_stream_and_packetize(); // 从GStreamer管道拉取编码后的视频数据，进行分包处理，并将数据包加入发送队列，同时关联ByteSpan以便跟踪发送和确认
  void poll_gstreamer_bus(); // 轮询GStreamer总线以处理事件和错误，确保编码过程的稳定性，并在必要时重启管道
  void restart_gstreamer_pipeline(const char * reason); // 重启GStreamer管道，通常在发生错误或需要重新初始化时调用，reason参数用于记录重启原因以便调试和日志记录
  void tx_loop(); // 发送线程的主循环，负责从发送队列中取出数据包，通过串口或MQTT发送，并根据发送结果更新跟踪信息和统计数据
  void display_loop(); // 显示线程的主循环，负责将处理后的图像显示在窗口中，提供实时预览功能，并根据options_中的设置调整显示内容和频率
  void appendStreamSpan(std::deque<ByteSpan> & spans, uint64_t track_id, size_t bytes); // 向ByteSpan队列中追加一个新的span，如果最后一个span的track_id相同则合并字节数，以便更高效地跟踪发送进度
  void consumeStreamSpans( 
    std::deque<ByteSpan> & spans,
    size_t bytes_to_consume,
    std::vector<ByteSpan> * consumed_spans); // 从ByteSpan队列中消费指定字节数，并将消费的span信息记录到consumed_spans中以便后续处理，通常在发送完成或丢弃数据包时调用
  void accountConsumedSpans(const std::vector<ByteSpan> & spans, bool sent, int64_t completion_ns); // 根据发送结果更新对应track_id的FrameTrack信息，记录已发送或已丢弃的字节数，并在该帧的所有数据都处理完成时计算并记录相关统计数据，如端到端延迟和丢包情况
  void LogLatencyStats(int64_t now_ns); // 根据当前时间和上次记录的时间间隔，决定是否记录一次端到端延迟统计数据，并将统计结果输出到日志中以便监控和调试
  void LogRuntimeStats(int64_t now_ns); // 根据当前时间和上次记录的时间间隔，决定是否记录一次运行时统计数据，如输入帧率、编码帧率、发送速率、丢包率等，并将统计结果输出到日志中以便监控和调试
  void retireFrameTrackIfDone(uint64_t track_id); // 检查指定track_id的FrameTrack是否已经完成发送（即已发送字节数加上已丢弃字节数达到编码字节数），如果完成则从active_frame_tracks_中移除该track，并记录相关统计数据以便监控和调试

  Options options_; // 编码器选项，包含各种参数设置以调整编码和发送行为

  GstElement * pipeline_; // GStreamer管道对象，负责整个视频编码流程的管理和数据流转
  GstElement * appsrc_; // GStreamer AppSrc元素，用于将预处理后的图像数据推送到管道中进行编码
  GstElement * appsink_; // GStreamer AppSink元素，用于从管道中拉取编码后的视频数据进行分包和发送
  GstBus * bus_; // GStreamer总线对象，用于监听管道事件和错误，确保编码过程的稳定性

  std::unique_ptr<sniper::sender::UsbCustomByteBlockSender> serial_sender_; // 串口发送器对象，用于通过串口发送编码后的视频数据，封装了串口通信的细节
  std::unique_ptr<sniper::sender::MqttCustomByteBlockSender> mqtt_sender_; // MQTT发送器对象，用于通过MQTT协议发送编码后的视频数据，封装了MQTT通信的细节

  uint64_t packet_sequence_id_ = 0; // 数据包序列ID，用于为每个发送的数据包分配一个唯一的标识，以便接收端进行重组和处理
  uint32_t frame_count_ = 0; // 处理的输入帧总数，用于统计和监控输入帧率
  int last_logged_tx_payload_frame_id_ = -1; // 上一次打印发送日志的负载帧ID，避免同一帧的每个分片都重复刷屏

  std::thread display_thread_; // 显示线程对象，负责实时显示处理后的图像，提供预览功能
  std::atomic<bool> display_running_;  // 显示线程运行状态标志，控制显示线程的启动和停止

  std::mutex frame_mutex_; // 保护显示相关数据的互斥锁，确保在多线程环境下对显示数据的安全访问
  cv::Mat display_raw_frame_; // 用于显示的原始帧
  cv::Mat display_roi_frame_; // 用于显示的感兴趣区域帧
  cv::Mat display_static_frame_; // 用于显示的静态背景帧
  cv::Mat display_frame_; // 用于显示的最终帧

  std::vector<uint8_t> stream_buffer_; // 用于临时存储从GStreamer管道拉取的编码后的视频数据，直到这些数据被分包并加入发送队列
  std::deque<ByteSpan> stream_buffer_spans_; // 与stream_buffer_对应的ByteSpan队列，用于跟踪stream_buffer_中每段数据对应的track_id和字节数，以便在发送时正确更新FrameTrack信息
  std::deque<TxPacket> tx_queue_; // 待发送的数据包队列，每个TxPacket包含一个数据包的负载和对应的ByteSpan列表，用于跟踪发送进度和结果
  std::deque<std::pair<int64_t, size_t>> sent_window_; // 发送窗口，用于记录最近发送的数据包的时间戳和字节数，以便计算当前的发送速率并根据options_中的带宽限制动态调整发送行为
  std::deque<cv::Mat> motion_mask_history_; // 运动掩码历史队列，用于实现运动拖尾效果，记录最近几帧的运动掩码以便在当前帧中合并使用
  std::deque<cv::Mat> trail_frame_history_; // 拖尾帧历史队列，用于实现运动拖尾效果，记录最近几帧的处理后图像以便在当前帧中合并使用
  std::unordered_map<uint64_t, PendingInputFrame> pending_input_frames_; // 待处理的输入帧信息表，key为frame_token，value为PendingInputFrame结构体，记录了输入帧的相关信息以便在编码和发送过程中进行关联和跟踪
  std::unordered_map<uint64_t, FrameTrack> active_frame_tracks_; // 活跃的帧跟踪表，key为track_id，value为FrameTrack结构体，记录了每个正在编码和发送的帧的跟踪信息，以便统计和监控每个帧的处理进度和结果
  size_t sent_window_bytes_ = 0; // 发送窗口中的总字节数，用于计算当前的发送速率并根据options_中的带宽限制动态调整发送行为
  uint64_t dropped_bytes_ = 0; // 累计丢弃的字节数，用于统计和监控由于过时或带宽限制等原因被丢弃的数据量，以便调整发送策略和优化性能
  uint32_t dropped_events_ = 0; // 累计丢弃事件的次数，用于统计和监控丢弃情况，以便调整发送策略和优化性能
  int64_t last_telemetry_ns_ = 0; // 上次记录统计数据的时间戳（纳秒），用于控制统计数据的记录频率，以避免过于频繁地记录导致性能问题
  int64_t last_latency_report_ns_ = 0; // 上次记录延迟统计数据的时间戳（纳秒），用于控制延迟统计数据的记录频率，以避免过于频繁地记录导致性能问题
  int64_t last_runtime_report_ns_ = 0; // 上次记录运行时统计数据的时间戳（纳秒），用于控制运行时统计数据的记录频率，以避免过于频繁地记录导致性能问题
  std::mutex buffer_mutex_; // 保护stream_buffer_、stream_buffer_spans_和tx_queue_等相关数据结构的互斥锁，确保在多线程环境下对这些数据的安全访问
  std::condition_variable tx_cv_; // 发送线程的条件变量，用于在有新的数据包加入tx_queue_时通知发送线程进行处理

  std::thread tx_thread_; // 发送线程对象，负责从tx_queue_中取出数据包进行发送，并根据发送结果更新跟踪信息和统计数据
  std::atomic<bool> tx_running_{false}; // 发送线程运行状态标志，控制发送线程的启动和停止
  int64_t next_tx_deadline_ns_ = 0; // 下一次发送的截止时间戳（纳秒），用于控制发送速率以满足options_中的带宽限制和最大发送延迟要求
  int64_t frame_interval_ns_ = 0; // 帧间隔时间（纳秒），根据options_中的output_fps计算得出，用于控制输入帧的处理速率和发送速率，以满足目标帧率要求
  int64_t stream_buffer_first_byte_ns_ = 0; // stream_buffer_中第一字节的时间戳（纳秒），用于计算数据包在队列中的等待时间，以便根据options_中的max_tx_delay_s丢弃过时的数据包 
  int64_t pipeline_started_ns_ = 0; // GStreamer管道启动的时间戳（纳秒），用于计算编码和发送过程中的各种延迟，以便统计和监控性能表现
  int64_t last_encoded_sample_ns_ = 0; // 上次成功从GStreamer管道拉取编码后的视频数据的时间戳（纳秒），用于计算编码帧率和监控编码过程的活跃度，以便在编码停滞时采取措施如重启管道
  uint64_t next_input_frame_token_ = 1; // 下一个输入帧令牌，用于为每个输入帧分配一个唯一的标识，以便在编码和发送过程中进行关联和跟踪
  uint64_t next_frame_track_id_ = 1; // 下一个帧跟踪ID，用于为每个正在编码和发送的帧分配一个唯一的标识，以便在统计和监控每个帧的处理进度和结果时进行关联和跟踪
  uint16_t next_payload_frame_id_ = 0; // 下一个负载帧ID，用于为每个编码后的视频帧分配一个唯一的标识，以便在发送时的标识和接收端的重组过程中进行关联和跟踪
  uint64_t input_frame_count_ = 0; // 处理的输入帧总数，用于统计和监控输入帧率
  uint64_t throttled_frame_count_ = 0; // 由于带宽限制或发送队列积压等原因被节流丢弃的输入帧数量，用于统计和监控丢弃情况，以便调整发送策略和优化性能
  uint64_t pushed_frame_count_ = 0; // 成功推送到GStreamer管道进行编码的输入帧数量，用于统计和监控编码帧率，以便调整预处理和发送策略以满足目标帧率要求
  uint64_t push_failed_count_ = 0; // 由于GStreamer管道状态或其他原因导致推送到管道失败的输入帧数量，用于统计和监控编码过程的稳定性，以便在发生推送失败时采取措施如重启管道
  uint64_t pulled_sample_count_ = 0; // 成功从GStreamer管道拉取编码后的视频数据的样本数量，用于统计和监控编码帧率，以便调整预处理和发送策略以满足目标帧率要求
  uint64_t sent_packet_count_ = 0; // 成功发送的数据包数量，用于统计和监控发送速率和带宽占用，以便调整发送策略以满足options_中的带宽限制要求
  uint64_t send_failed_count_ = 0; // 由于串口或MQTT通信错误等原因导致发送失败的数据包数量，用于统计和监控发送过程的稳定性，以便在发生发送失败时采取措施如重启发送器或调整发送策略
  uint64_t latency_sample_count_ = 0; // 端到端延迟样本数量，用于统计和监控从输入帧到数据包发送完成的延迟表现，以便调整预处理、编码和发送策略以优化性能
  double latency_sum_ms_ = 0.0; // 端到端延迟总和（毫秒），用于计算平均延迟以统计和监控性能表现
  double latency_latest_ms_ = 0.0; // 最近一次计算的端到端延迟（毫秒），用于统计和监控性能表现
  double latency_min_ms_ = -1.0; // 端到端延迟最小值（毫秒），用于统计和监控性能表现
  double latency_max_ms_ = 0.0; // 端到端延迟最大值（毫秒），用于统计和监控性能表现
  uint64_t latency_latest_frame_id_ = 0; // 最近一次计算的端到端延迟对应的输入帧ID，用于统计和监控性能表现

  int64_t last_encode_stamp_ns_ = 0; // 上次成功编码的时间戳（纳秒），用于计算编码帧率和监控编码过程的活跃度，以便在编码停滞时采取措施如重启管道
  uint64_t display_frame_counter_ = 0; // 显示帧计数器，用于控制调试图像的保存频率，以避免过于频繁地保存导致性能问题
  cv::Mat background_gray_f32_; // 背景图像的灰度浮点版本，用于背景建模和运动检测，以便实现静态区域简化和运动拖尾效果
  cv::Mat motion_erode_kernel_; // 运动掩码腐蚀核，用于去除运动检测中的噪点，以便提高编码效率和视频质量
  cv::Mat motion_dilate_kernel_; // 运动掩码膨胀核，用于连接运动检测中的断开区域，以便提高编码效率和视频质量
};

}  // namespace doorlock_sniper

#endif  // SNIPER_ENCODER_HPP_
