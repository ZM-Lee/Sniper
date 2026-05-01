#include "sender/mqtt.hpp"

#include "protobuf/rmcc.pb.hpp"

#include <cstring>

#include <mosquitto.h>

namespace sniper::sender
{

MqttCustomByteBlockSender::MqttCustomByteBlockSender(
	std::string host,
	const int port,
	std::string topic,
	std::string client_id,
	const int qos)
: host_(std::move(host)),
	port_(port),
	topic_(std::move(topic)),
	client_id_(std::move(client_id)),
	qos_(qos),
	connected_(false),
	lib_initialized_(false),
	mosq_(nullptr),
	last_error_()
{
}

MqttCustomByteBlockSender::~MqttCustomByteBlockSender()
{
	close();
}

bool MqttCustomByteBlockSender::open()
{
	std::lock_guard<std::mutex> lock(mutex_);
	close();
	last_error_.clear();

	const int lib_rc = mosquitto_lib_init();
	if (lib_rc != MOSQ_ERR_SUCCESS) {
		last_error_ = std::string("mosquitto_lib_init failed: ") + mosquitto_strerror(lib_rc);
		return false;
	}
	lib_initialized_ = true;

	mosq_ = mosquitto_new(client_id_.empty() ? nullptr : client_id_.c_str(), true, nullptr);
	if (mosq_ == nullptr) {
		last_error_ = "mosquitto_new failed";
		mosquitto_lib_cleanup();
		lib_initialized_ = false;
		return false;
	}

	// Prefer MQTT v3.1.1 for compatibility with most embedded brokers.
	mosquitto_int_option(mosq_, MOSQ_OPT_PROTOCOL_VERSION, MQTT_PROTOCOL_V311);

	const int conn_rc = mosquitto_connect(mosq_, host_.c_str(), port_, 60);
	if (conn_rc != MOSQ_ERR_SUCCESS) {
		if (conn_rc == MOSQ_ERR_ERRNO) {
			last_error_ = std::string("mosquitto_connect errno: ") + std::strerror(errno);
		} else {
			last_error_ = std::string("mosquitto_connect failed: ") + mosquitto_strerror(conn_rc);
		}
		mosquitto_destroy(mosq_);
		mosq_ = nullptr;
		mosquitto_lib_cleanup();
		lib_initialized_ = false;
		return false;
	}

	const int loop_rc = mosquitto_loop_start(mosq_);
	if (loop_rc != MOSQ_ERR_SUCCESS) {
		last_error_ = std::string("mosquitto_loop_start failed: ") + mosquitto_strerror(loop_rc);
		mosquitto_disconnect(mosq_);
		mosquitto_destroy(mosq_);
		mosq_ = nullptr;
		mosquitto_lib_cleanup();
		lib_initialized_ = false;
		return false;
	}

	connected_ = true;
	return true;
}

void MqttCustomByteBlockSender::close()
{
	if (mosq_ != nullptr) {
		mosquitto_loop_stop(mosq_, true);
		mosquitto_disconnect(mosq_);
		mosquitto_destroy(mosq_);
		mosq_ = nullptr;
	}
	connected_ = false;

	if (lib_initialized_) {
		mosquitto_lib_cleanup();
		lib_initialized_ = false;
	}
}

bool MqttCustomByteBlockSender::isOpen() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return connected_ && mosq_ != nullptr;
}

bool MqttCustomByteBlockSender::sendPayload(const std::vector<uint8_t> & payload)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!connected_ || mosq_ == nullptr) {
		last_error_ = "mqtt sender not connected";
		return false;
	}

	CustomByteBlock msg;
	if (payload.empty()) {
		msg.set_data(std::string{});
	} else {
		msg.set_data(std::string(reinterpret_cast<const char *>(payload.data()), payload.size()));
	}

	std::string serialized;
	if (!msg.SerializeToString(&serialized)) {
		last_error_ = "CustomByteBlock SerializeToString failed";
		return false;
	}

	const int rc = mosquitto_publish(
		mosq_,
		nullptr,
		topic_.c_str(),
		static_cast<int>(serialized.size()),
		serialized.empty() ? nullptr : serialized.data(),
		qos_,
		false);
	if (rc != MOSQ_ERR_SUCCESS) {
		last_error_ = std::string("mosquitto_publish failed: ") + mosquitto_strerror(rc);
	}
	return rc == MOSQ_ERR_SUCCESS;
}

const std::string & MqttCustomByteBlockSender::lastError() const
{
	return last_error_;
}

}  // namespace sniper::sender
