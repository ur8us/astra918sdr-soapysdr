#pragma once

#include "astra918/protocol.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace astra918 {

struct DeviceInfo {
  std::string serial;
  std::string label;
  std::uint8_t bus = 0;
  std::uint8_t address = 0;
};

class Transport {
public:
  virtual ~Transport() = default;
  virtual Bytes command(Command command, const Bytes &payload = {}) = 0;
  virtual std::size_t readIq(std::uint8_t *buffer, std::size_t capacity,
                             unsigned timeoutMs) = 0;
  virtual void drainIq() = 0;
};

class UsbTransport final : public Transport {
public:
  static std::vector<DeviceInfo> enumerate();
  static std::unique_ptr<UsbTransport> open(const std::string &serial);
  ~UsbTransport() override;

  Bytes command(Command command, const Bytes &payload = {}) override;
  std::size_t readIq(std::uint8_t *buffer, std::size_t capacity,
                     unsigned timeoutMs) override;
  void drainIq() override;

  const DeviceInfo &info() const { return info_; }

private:
  UsbTransport(void *context, void *handle, DeviceInfo info);
  void *context_ = nullptr;
  void *handle_ = nullptr;
  DeviceInfo info_;
  std::mutex commandMutex_;
  std::uint32_t sequence_ = 0;
  bool controlUsable_ = true;
};

} // namespace astra918
