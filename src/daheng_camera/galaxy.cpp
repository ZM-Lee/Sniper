#include "daheng_camera/galaxy.hpp"

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>

#include <opencv2/imgproc.hpp>
#include <yaml-cpp/yaml.h>

namespace sniper::daheng_camera
{
namespace
{

int64_t NowNs()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch())
		.count();
}

void ThrowIfFailed(const GX_STATUS status, const char * message)
{
	if (status != GX_STATUS_SUCCESS) {
		throw std::runtime_error(message);
	}
}

int ToCvtCode(const int bayer_format_index)
{
	switch (bayer_format_index) {
		case 0:
			return cv::COLOR_BayerBG2BGR;  // RG8 -> BGR
		case 1:
			return cv::COLOR_BayerGB2BGR;  // GR8 -> BGR
		case 2:
			return cv::COLOR_BayerGR2BGR;  // GB8 -> BGR
		case 3:
			return cv::COLOR_BayerRG2BGR;  // BG8 -> BGR
		default:
			return cv::COLOR_BayerBG2BGR;
	}
}

}  // namespace

GalaxyCamera::GalaxyCamera()
: device_handle_(nullptr),
	payload_size_(0),
	width_(0),
	height_(0),
	bayer_format_index_(0),
	lib_initialized_(false),
	stream_on_(false),
	timestamp_calibrated_(false),
	timestamp_tick_frequency_(0),
	timestamp_base_camera_ticks_(0),
	timestamp_base_host_ns_(0)
{
}

GalaxyCamera::~GalaxyCamera()
{
	close();
}

bool GalaxyCamera::open(const Config & config)
{
	close();

	try {
		ThrowIfFailed(GXInitLib(), "Failed to initialize Daheng library");
		lib_initialized_ = true;

		uint32_t device_count = 0;
		ThrowIfFailed(GXUpdateDeviceList(&device_count, 1000), "Failed to update Daheng device list");
		if (device_count == 0U) {
			throw std::runtime_error("No Daheng camera found");
		}

		ThrowIfFailed(GXOpenDeviceByIndex(1, &device_handle_), "Failed to open Daheng camera");

		applyConfig(config);

		int64_t width_val = 0;
		int64_t height_val = 0;
		ThrowIfFailed(GXGetInt(device_handle_, GX_INT_WIDTH, &width_val), "Failed to get camera width");
		ThrowIfFailed(GXGetInt(device_handle_, GX_INT_HEIGHT, &height_val), "Failed to get camera height");
		ThrowIfFailed(
			GXGetInt(device_handle_, GX_INT_PAYLOAD_SIZE, &payload_size_),
			"Failed to get camera payload size");

		width_ = static_cast<int>(width_val);
		height_ = static_cast<int>(height_val);
		frame_buffer_.resize(static_cast<size_t>(payload_size_));

		selectBayerFormat();
		calibrateTimestampClock();

		ThrowIfFailed(GXStreamOn(device_handle_), "Failed to start Daheng stream");
		stream_on_ = true;
		return true;
	} catch (const std::exception & e) {
		std::cerr << "[GalaxyCamera] open failed: " << e.what() << std::endl;
		close();
		return false;
	}
}

bool GalaxyCamera::openFromYaml(const std::string & yaml_path)
{
	try {
		const YAML::Node node = YAML::LoadFile(yaml_path);

		Config config;
		if (node["ExposureTime"]) {
			config.exposure_time = node["ExposureTime"].as<double>();
		}
		if (node["Gain"]) {
			config.gain = node["Gain"].as<double>();
		}
		if (node["Width"]) {
			config.width = node["Width"].as<int>();
		}
		if (node["Height"]) {
			config.height = node["Height"].as<int>();
		}
		if (node["AcquisitionFrameRate"]) {
			config.acquisition_fps = node["AcquisitionFrameRate"].as<double>();
		}
		if (node["BalanceWhiteAuto"]) {
			config.balance_white_auto = (node["BalanceWhiteAuto"].as<int>() != 0);
		}
		if (node["TriggerMode"]) {
			config.trigger_mode = (node["TriggerMode"].as<int>() != 0);
		}
		if (node["GammaEnable"]) {
			config.gamma_enable = (node["GammaEnable"].as<int>() != 0);
		}
		if (node["GammaMode"]) {
			config.gamma_mode = node["GammaMode"].as<int>();
		}
		if (node["Gamma"]) {
			config.gamma = node["Gamma"].as<double>();
		}
		if (node["GammaParam"]) {
			config.gamma_param = node["GammaParam"].as<double>();
		}

		return open(config);
	} catch (const std::exception & e) {
		std::cerr << "[GalaxyCamera] failed to parse config " << yaml_path << ": " << e.what() << std::endl;
		return false;
	}
}

bool GalaxyCamera::read(cv::Mat & bgr_image, FrameInfo * frame_info, const int timeout_ms)
{
	if (!stream_on_ || device_handle_ == nullptr || frame_buffer_.empty() || width_ <= 0 || height_ <= 0) {
		return false;
	}

	GX_FRAME_DATA frame_data;
	frame_data.pImgBuf = frame_buffer_.data();

	const GX_STATUS status = GXGetImage(device_handle_, &frame_data, timeout_ms);
	if (status != GX_STATUS_SUCCESS || frame_data.nStatus != GX_FRAME_STATUS_SUCCESS) {
		return false;
	}
	const int64_t host_receive_ns = NowNs();

	if (frame_info != nullptr) {
		frame_info->frame_id = frame_data.nFrameID;
		frame_info->camera_timestamp_ticks = frame_data.nTimestamp;
		frame_info->host_receive_time_ns = host_receive_ns;
		frame_info->capture_time_ns = host_receive_ns;
		frame_info->capture_time_valid = true;
		frame_info->hardware_timestamp_valid = (frame_data.nTimestamp != 0U);

		const int64_t mapped_capture_ns = cameraTicksToHostNs(frame_data.nTimestamp);
		if (mapped_capture_ns > 0) {
			frame_info->capture_time_ns = mapped_capture_ns;
		}
	}

	cv::Mat bayer(height_, width_, CV_8UC1, frame_buffer_.data());
	cv::cvtColor(bayer, bgr_image, ToCvtCode(bayer_format_index_));
	return true;
}

void GalaxyCamera::close()
{
	if (stream_on_ && device_handle_ != nullptr) {
		GXStreamOff(device_handle_);
		stream_on_ = false;
	}

	if (device_handle_ != nullptr) {
		GXCloseDevice(device_handle_);
		device_handle_ = nullptr;
	}

	if (lib_initialized_) {
		GXCloseLib();
		lib_initialized_ = false;
	}

	frame_buffer_.clear();
	payload_size_ = 0;
	width_ = 0;
	height_ = 0;
	timestamp_calibrated_ = false;
	timestamp_tick_frequency_ = 0;
	timestamp_base_camera_ticks_ = 0;
	timestamp_base_host_ns_ = 0;
}

bool GalaxyCamera::isOpened() const
{
	return stream_on_ && device_handle_ != nullptr;
}

int GalaxyCamera::width() const
{
	return width_;
}

int GalaxyCamera::height() const
{
	return height_;
}

void GalaxyCamera::applyConfig(const Config & config)
{
	if (config.width > 0) {
		GXSetInt(device_handle_, GX_INT_WIDTH, config.width);
	}
	if (config.height > 0) {
		GXSetInt(device_handle_, GX_INT_HEIGHT, config.height);
	}

	GXSetEnum(device_handle_, GX_ENUM_EXPOSURE_AUTO, GX_EXPOSURE_AUTO_OFF);
	if (config.exposure_time > 0.0) {
		GXSetFloat(device_handle_, GX_FLOAT_EXPOSURE_TIME, config.exposure_time);
	}

	if (config.gain >= 0.0) {
		GXSetFloat(device_handle_, GX_FLOAT_GAIN, config.gain);
	}

	GXSetEnum(
		device_handle_,
		GX_ENUM_BALANCE_WHITE_AUTO,
		config.balance_white_auto ? GX_BALANCE_WHITE_AUTO_CONTINUOUS : GX_BALANCE_WHITE_AUTO_OFF);

	GXSetEnum(
		device_handle_,
		GX_ENUM_TRIGGER_MODE,
		config.trigger_mode ? GX_TRIGGER_MODE_ON : GX_TRIGGER_MODE_OFF);

	GXSetBool(device_handle_, GX_BOOL_GAMMA_ENABLE, config.gamma_enable);
	if (config.gamma_enable) {
		GXSetEnum(device_handle_, GX_ENUM_GAMMA_MODE, config.gamma_mode);
		if (config.gamma_mode == GX_GAMMA_SELECTOR_USER && config.gamma > 0.0) {
			GXSetFloat(device_handle_, GX_FLOAT_GAMMA, config.gamma);
		}
	}

	GXSetEnum(device_handle_, GX_ENUM_ACQUISITION_MODE, GX_ACQ_MODE_CONTINUOUS);

	if (config.acquisition_fps > 0.0) {
		GXSetEnum(device_handle_, GX_ENUM_ACQUISITION_FRAME_RATE_MODE, GX_ACQUISITION_FRAME_RATE_MODE_ON);
		GXSetFloat(device_handle_, GX_FLOAT_ACQUISITION_FRAME_RATE, config.acquisition_fps);
	}
}

void GalaxyCamera::selectBayerFormat()
{
	const std::array<GX_PIXEL_FORMAT_ENTRY, 4> bayer_formats = {
		GX_PIXEL_FORMAT_BAYER_RG8,
		GX_PIXEL_FORMAT_BAYER_GR8,
		GX_PIXEL_FORMAT_BAYER_GB8,
		GX_PIXEL_FORMAT_BAYER_BG8};

	bayer_format_index_ = 0;
	for (size_t i = 0; i < bayer_formats.size(); ++i) {
		if (GXSetEnum(device_handle_, GX_ENUM_PIXEL_FORMAT, bayer_formats[i]) == GX_STATUS_SUCCESS) {
			bayer_format_index_ = static_cast<int>(i);
			return;
		}
	}
}

void GalaxyCamera::calibrateTimestampClock()
{
	timestamp_calibrated_ = false;
	timestamp_tick_frequency_ = 0;
	timestamp_base_camera_ticks_ = 0;
	timestamp_base_host_ns_ = 0;

	if (device_handle_ == nullptr) {
		return;
	}

	int64_t tick_frequency = 0;
	if (GXGetInt(device_handle_, GX_INT_TIMESTAMP_TICK_FREQUENCY, &tick_frequency) != GX_STATUS_SUCCESS ||
		tick_frequency <= 0) {
		return;
	}

	const int64_t host_before_ns = NowNs();
	if (GXSendCommand(device_handle_, GX_COMMAND_TIMESTAMP_LATCH) != GX_STATUS_SUCCESS) {
		return;
	}

	int64_t latched_ticks = 0;
	if (GXGetInt(device_handle_, GX_INT_TIMESTAMP_LATCH_VALUE, &latched_ticks) != GX_STATUS_SUCCESS ||
		latched_ticks < 0) {
		return;
	}
	const int64_t host_after_ns = NowNs();

	timestamp_tick_frequency_ = static_cast<uint64_t>(tick_frequency);
	timestamp_base_camera_ticks_ = static_cast<uint64_t>(latched_ticks);
	timestamp_base_host_ns_ = host_before_ns + (host_after_ns - host_before_ns) / 2;
	timestamp_calibrated_ = true;
}

int64_t GalaxyCamera::cameraTicksToHostNs(const uint64_t camera_ticks) const
{
	if (!timestamp_calibrated_ || timestamp_tick_frequency_ == 0U || camera_ticks < timestamp_base_camera_ticks_) {
		return 0;
	}

	const uint64_t delta_ticks = camera_ticks - timestamp_base_camera_ticks_;
	const long double delta_ns =
		static_cast<long double>(delta_ticks) * 1000000000.0L /
		static_cast<long double>(timestamp_tick_frequency_);
	return timestamp_base_host_ns_ + static_cast<int64_t>(delta_ns);
}

}  // namespace sniper::daheng_camera
