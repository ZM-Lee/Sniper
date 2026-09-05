#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <stdexcept>

#include <opencv2/opencv.hpp>
#include <yaml-cpp/yaml.h>

#include "daheng_camera/galaxy.hpp"
#include "encoder.hpp"

namespace
{

std::atomic<bool> g_running{true};

constexpr const char * kTransport = "mqtt";  // "serial" or "mqtt"
constexpr const char * kCameraConfig = "../config/daheng_camera/feature.yaml";
constexpr const char * kMqttConfig = "../config/mqtt/server.yaml";
constexpr const char * kSerialConfig = "../config/serial/serial.yaml";

constexpr const char * kSerialPort = "/dev/ttyUSB0";
constexpr int kSerialBaud = 921600;

constexpr const char * kMqttHost = "127.0.0.1";
constexpr int kMqttPort = 3333;
constexpr const char * kMqttTopic = "CustomByteBlock";
constexpr const char * kMqttClientId = "doorlock_sniper";
constexpr int kMqttQos = 0;

constexpr bool kEnableDisplay = true; // 调试窗口

void OnSignal(int)
{
  g_running = false;
}

std::string ResolveCameraConfigPath(const std::string & input_path)
{
  if (!input_path.empty()) {
    return input_path;
  }
  
  const std::string candidates[] = {"../config/daheng_camera/feature.yaml"};

  for (const auto & p : candidates) {
    if (std::filesystem::exists(p)) {
      return p;
    }
  }
  return candidates[0];
}

}  // namespace

int main()
{
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  doorlock_sniper::VideoEncoder::Options encoder_options;
  encoder_options.enable_display = kEnableDisplay;
  encoder_options.serial_port = kSerialPort;
  encoder_options.serial_baudrate = kSerialBaud;
  encoder_options.mqtt_host = kMqttHost;
  encoder_options.mqtt_port = kMqttPort;
  encoder_options.mqtt_topic = kMqttTopic;
  encoder_options.mqtt_client_id = kMqttClientId;
  encoder_options.mqtt_qos = kMqttQos;

  std::string transport = kTransport;

  std::transform(
    transport.begin(), transport.end(), transport.begin(),
    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  encoder_options.send_chain = transport;

  if (transport == "serial") {
    const std::string serial_config = kSerialConfig;
    if (std::filesystem::exists(serial_config)) {
      try {
        const YAML::Node serial = YAML::LoadFile(serial_config);
        if (serial["serial_port"]) {
          encoder_options.serial_port = serial["serial_port"].as<std::string>();
        }
        if (serial["baud_rate"]) {
          encoder_options.serial_baudrate = serial["baud_rate"].as<int>();
        }
      } catch (const std::exception & e) {
        std::cerr << "Failed to parse serial config " << serial_config << ": " << e.what() << std::endl;
        return 1;
      }
    } else {
      std::cerr << "Serial config not found: " << serial_config << std::endl;
      return 1;
    }
  } else if (transport == "mqtt") {
    const std::string mqtt_config = kMqttConfig;
    if (std::filesystem::exists(mqtt_config)) {
      try {
        const YAML::Node mqtt = YAML::LoadFile(mqtt_config);
        if (mqtt["ip"]) {
          encoder_options.mqtt_host = mqtt["ip"].as<std::string>();
        }
        if (mqtt["port"]) {
          encoder_options.mqtt_port = mqtt["port"].as<int>();
        }
        if (mqtt["topic"]) {
          encoder_options.mqtt_topic = mqtt["topic"].as<std::string>();
        }
      } catch (const std::exception & e) {
        std::cerr << "Failed to parse mqtt config " << mqtt_config << ": " << e.what() << std::endl;
        return 1;
      }
    } else {
      std::cerr << "MQTT config not found: " << mqtt_config << std::endl;
      return 1;
    }
  } else {
    std::cerr << "Unsupported transport" << std::endl;
    return 1;
  }

  const std::string camera_path = ResolveCameraConfigPath(kCameraConfig);
  sniper::daheng_camera::GalaxyCamera camera;
  if (!camera.openFromYaml(camera_path)) {
    std::cerr << "Failed to open Daheng camera with config: " << camera_path << std::endl;
    return 1;
  }

  std::cout << "Camera opened: " << camera.width() << "x" << camera.height() << std::endl;
  std::cout << "Transport: " << encoder_options.send_chain << std::endl;

  try {
    doorlock_sniper::VideoEncoder encoder(encoder_options);

    cv::Mat frame;
    sniper::daheng_camera::GalaxyCamera::FrameInfo frame_info;
    uint64_t read_timeout_count = 0;
    uint64_t read_success_count = 0;
    while (g_running) {
      if (camera.read(frame, &frame_info, 100) && !frame.empty()) {
        ++read_success_count;
        const int64_t capture_time_ns =
          (frame_info.capture_time_valid && frame_info.capture_time_ns > 0)
          ? frame_info.capture_time_ns
          : frame_info.host_receive_time_ns;
        encoder.processImage(
          frame,
          capture_time_ns,
          frame_info.host_receive_time_ns,
          frame_info.frame_id);
      } else {
        ++read_timeout_count;
        if ((read_timeout_count % 50U) == 0U) {
          std::cerr
            << "[Sniper][WARN] camera.read timeout/failure count=" << read_timeout_count
            << " success=" << read_success_count
            << std::endl;
        }
      }
    }
  } catch (const std::exception & e) {
    std::cerr << "Encoder startup/runtime error: " << e.what() << std::endl;
    camera.close();
    return 1;
  }

  camera.close();
  return 0;
}
