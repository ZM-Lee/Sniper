#ifndef SNIPER_SENDER_SERIAL_HPP_
#define SNIPER_SENDER_SERIAL_HPP_

#include <cstddef>
#include <cstdint> 
#include <string>
#include <vector>

namespace sniper::sender
{

class UsbCustomByteBlockSender
{
public:
  UsbCustomByteBlockSender(std::string port, int baudrate, uint16_t cmd_id = 0x0310);
  ~UsbCustomByteBlockSender();

  bool open();
  void close();
  bool isOpen() const;

  std::vector<uint8_t> buildCustomFrame(uint16_t cmd_id, const std::vector<uint8_t> & payload) const;
  std::vector<uint8_t> buildSerialFrame(const std::vector<uint8_t> & payload);

  bool sendFrame(const std::vector<uint8_t> & frame);
  bool sendOnce(const std::vector<uint8_t> & payload, size_t * written_bytes = nullptr);

  uint8_t sequence() const;
  uint16_t cmdId() const;
  const std::string & port() const;
  int baudrate() const;

private:
  bool configurePort();

  int fd_;
  std::string port_;
  int baudrate_;
  uint8_t seq_;
  uint16_t cmd_id_;
};

}  // namespace sniper::sender

#endif  // SNIPER_SENDER_SERIAL_HPP_
