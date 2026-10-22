#include "Common/FloatUtils.h"

#include <array>
#include <bit>
#include <cstdint>
#include <iomanip>
#include <iostream>

int main() {
  const std::array<std::uint64_t, 14> values = {
      0x0000000000000000ULL,0x8000000000000000ULL,0x3ff0000000000000ULL,
      0x4000000000000000ULL,0x3fd5555555555555ULL,0x0010000000000000ULL,
      0x0000000000000001ULL,0x7fefffffffffffffULL,0x7ff0000000000000ULL,
      0xfff0000000000000ULL,0x7ff0000000000001ULL,0x7ff8000012345678ULL,
      0xbff0000000000000ULL,0x4010000000000000ULL};
  std::cout << std::hex << std::setfill('0');
  for (const auto bits : values) {
    const double input = std::bit_cast<double>(bits);
    std::cout << "0x" << std::setw(16) << bits << " 0x" << std::setw(16)
              << std::bit_cast<std::uint64_t>(Common::ApproximateReciprocal(input))
              << " 0x" << std::setw(16)
              << std::bit_cast<std::uint64_t>(Common::ApproximateReciprocalSquareRoot(input))
              << '\n';
  }
}
