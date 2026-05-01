#ifndef SNIPER_SENDER_MQTT_HPP_
#define SNIPER_SENDER_MQTT_HPP_

#include <mutex>
#include <string>
#include <vector>

struct mosquitto;

namespace sniper::sender
{

class MqttCustomByteBlockSender
{
public:
  MqttCustomByteBlockSender(
    std::string host,
    int port,
    std::string topic,
    std::string client_id,
    int qos = 1);
  ~MqttCustomByteBlockSender();

  bool open();
  void close();
  bool isOpen() const;
  const std::string & lastError() const;

  // Publish only payload bytes as MQTT message body.
  bool sendPayload(const std::vector<uint8_t> & payload);

private:
  std::string host_;
  int port_;
  std::string topic_;
  std::string client_id_;
  int qos_;

  bool connected_;
  bool lib_initialized_;
  mosquitto * mosq_;
  std::string last_error_;
  mutable std::mutex mutex_;
};

}  // namespace sniper::sender

#endif  // SNIPER_SENDER_MQTT_HPP_
