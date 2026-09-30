#pragma once

#include <cstdint>
#include <string>

namespace astra918 {

// Gqrx's F command changes its demodulator offset before retuning its RF
// center. These commands leave the RF center at centerHz and the demodulator
// one hertz away; zero offset is not attainable through Gqrx's remote API.
struct GqrxTunePlan {
  std::uint64_t resetHz;
  std::uint64_t centerCommandHz;
  std::uint64_t finalHz;
};

GqrxTunePlan planGqrxCenter(std::uint64_t centerHz);
bool syncGqrxCenter(std::uint16_t port, std::uint64_t centerHz,
                    std::string &error);

} // namespace astra918
