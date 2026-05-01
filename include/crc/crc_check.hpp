#ifndef SNIPER_CRC_CRC_CHECK_HPP_
#define SNIPER_CRC_CRC_CHECK_HPP_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sniper::crc
{

uint8_t crc8(const uint8_t * data, size_t length);
uint16_t crc16(const uint8_t * data, size_t length);

uint8_t crc8(const std::vector<uint8_t> & data);
uint16_t crc16(const std::vector<uint8_t> & data);

bool verify_crc8(const uint8_t * data, size_t length, uint8_t expected_crc);
bool verify_crc16(const uint8_t * data, size_t length, uint16_t expected_crc);

bool verify_crc8(const std::vector<uint8_t> & data, uint8_t expected_crc);
bool verify_crc16(const std::vector<uint8_t> & data, uint16_t expected_crc);

}  // namespace sniper::crc

#endif  // SNIPER_CRC_CRC_CHECK_HPP_
