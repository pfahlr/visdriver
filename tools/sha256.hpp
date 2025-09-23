#ifndef TOOLS_SHA256_HPP_
#define TOOLS_SHA256_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class Sha256 {
 public:
  Sha256();

  void Update(const uint8_t *data, size_t len);
  void Update(const std::vector<uint8_t> &data);

  std::array<uint8_t, 32> Final();

 private:
  void Transform(const uint8_t block[64]);

  bool finalized_;
  uint64_t bit_count_;
  std::array<uint32_t, 8> state_;
  std::array<uint8_t, 64> buffer_;
  size_t buffer_size_;
  std::array<uint8_t, 32> final_digest_;
};

std::array<uint8_t, 32> Sha256Hash(const uint8_t *data, size_t len);
std::array<uint8_t, 32> Sha256Hash(const std::vector<uint8_t> &data);
std::string Sha256ToHex(const std::array<uint8_t, 32> &digest);
std::string Sha256ToHex(const uint8_t digest[32]);

#endif  // TOOLS_SHA256_HPP_
