#include "sender/serial.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <iostream>

#include "crc/crc_check.hpp"

namespace sniper::sender
{
namespace
{

speed_t ToBaudFlag(const int baudrate)
{
  switch (baudrate) {
    case 9600:
      return B9600;
    case 19200:
      return B19200;
    case 38400:
      return B38400;
    case 57600:
      return B57600;
    case 115200:
      return B115200;
    case 230400:
      return B230400;
    case 460800:
      return B460800;
    case 921600:
      return B921600;
    default:
      throw std::invalid_argument("Unsupported baudrate");
  }
}

}  // namespace

UsbCustomByteBlockSender::UsbCustomByteBlockSender(std::string port, const int baudrate, const uint16_t cmd_id)
: fd_(-1),
  port_(std::move(port)),
  baudrate_(baudrate),
  seq_(0),
  cmd_id_(cmd_id)
{
}

UsbCustomByteBlockSender::~UsbCustomByteBlockSender()
{
  close();
}

bool UsbCustomByteBlockSender::open()
{
  close();

  fd_ = ::open(port_.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
  if (fd_ < 0) {
    return false;
  }

  if (!configurePort()) {
    close();
    return false;
  }
  return true;
}

void UsbCustomByteBlockSender::close()
{
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

bool UsbCustomByteBlockSender::isOpen() const
{
  return fd_ >= 0;
}

void printBytes(const std::vector<uint8_t>& buf)
{
  for (auto byte : buf)
  {
    std::cout<< std::hex<<(int)byte<<"";
  }
  std::cout<< std::dec<<std::endl;

}

std::vector<uint8_t> UsbCustomByteBlockSender::buildCustomFrame(
  const uint16_t cmd_id,
  const std::vector<uint8_t> & payload) const
{
  const uint16_t data_length = static_cast<uint16_t>(payload.size());

  std::vector<uint8_t> frame;
  frame.reserve(static_cast<size_t>(9) + payload.size());

  frame.push_back(0xA5);
  frame.push_back(static_cast<uint8_t>(data_length & 0xFF));
  frame.push_back(static_cast<uint8_t>((data_length >> 8) & 0xFF));
  frame.push_back(seq_);

  const uint8_t crc8_value = crc::crc8(frame.data(), frame.size());
  frame.push_back(crc8_value);

  frame.push_back(static_cast<uint8_t>(cmd_id & 0xFF));
  frame.push_back(static_cast<uint8_t>((cmd_id >> 8) & 0xFF));

  frame.insert(frame.end(), payload.begin(), payload.end());

  const uint16_t crc16_value = crc::crc16(frame.data(), frame.size());
  frame.push_back(static_cast<uint8_t>(crc16_value & 0xFF));
  frame.push_back(static_cast<uint8_t>((crc16_value >> 8) & 0xFF));
  // printBytes(frame);

  return frame;
}

std::vector<uint8_t> UsbCustomByteBlockSender::buildSerialFrame(const std::vector<uint8_t> & payload)
{
  if (payload.size() > 300U) {
    throw std::invalid_argument("payload too long (>300)");
  }

  seq_ = static_cast<uint8_t>((seq_ + 1U) & 0xFFU);
  return buildCustomFrame(cmd_id_, payload);
}

bool UsbCustomByteBlockSender::sendFrame(const std::vector<uint8_t> & frame)
{
  if (!isOpen()) {
    return false;
  }

  size_t total_written = 0;
  while (total_written < frame.size()) {
    const ssize_t n = ::write(fd_, frame.data() + total_written, frame.size() - total_written);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    total_written += static_cast<size_t>(n);
  }

  return (::tcdrain(fd_) == 0);
}

bool UsbCustomByteBlockSender::sendOnce(const std::vector<uint8_t> & payload, size_t * written_bytes)
{
  const auto frame = buildSerialFrame(payload);
  const bool ok = sendFrame(frame);
  if (written_bytes != nullptr) {
    *written_bytes = ok ? frame.size() : 0U;
  }
  return ok;
}

uint8_t UsbCustomByteBlockSender::sequence() const
{
  return seq_;
}

uint16_t UsbCustomByteBlockSender::cmdId() const
{
  return cmd_id_;
}

const std::string & UsbCustomByteBlockSender::port() const
{
  return port_;
}

int UsbCustomByteBlockSender::baudrate() const
{
  return baudrate_;
}

bool UsbCustomByteBlockSender::configurePort()
{
  termios tty;
  if (::tcgetattr(fd_, &tty) != 0) {
    return false;
  }

  const speed_t baud_flag = ToBaudFlag(baudrate_);
  ::cfsetospeed(&tty, baud_flag);
  ::cfsetispeed(&tty, baud_flag);

  tty.c_cflag = static_cast<tcflag_t>((tty.c_cflag & ~CSIZE) | CS8);
  tty.c_cflag |= (CLOCAL | CREAD);
  tty.c_cflag &= static_cast<tcflag_t>(~(PARENB | PARODD | CSTOPB | CRTSCTS));

  tty.c_iflag &= static_cast<tcflag_t>(~(IXON | IXOFF | IXANY | IGNBRK));
  tty.c_lflag = 0;
  tty.c_oflag = 0;

  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 2;  // 200 ms, matching previous timeout behavior

  if (::tcsetattr(fd_, TCSANOW, &tty) != 0) {
    return false;
  }

  return true;
}

}  // namespace sniper::sender
