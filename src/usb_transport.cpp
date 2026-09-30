#include "usb_transport.hpp"

#include <libusb.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstring>
#include <sstream>
#include <thread>

namespace astra918 {
namespace {

libusb_context *context(void *value) {
  return static_cast<libusb_context *>(value);
}
libusb_device_handle *handle(void *value) {
  return static_cast<libusb_device_handle *>(value);
}

std::string usbError(const std::string &operation, const int code) {
  return operation + ": " + libusb_error_name(code);
}

std::string readString(libusb_device_handle *device, const std::uint8_t index) {
  if (index == 0)
    return {};
  std::array<unsigned char, 256> buffer{};
  const int size = libusb_get_string_descriptor_ascii(
      device, index, buffer.data(), buffer.size());
  if (size <= 0)
    return {};
  return std::string(reinterpret_cast<char *>(buffer.data()),
                     static_cast<std::size_t>(size));
}

std::string fallbackSerial(const std::uint8_t bus, const std::uint8_t address) {
  std::ostringstream text;
  text << "bus-" << static_cast<unsigned>(bus) << "-address-"
       << static_cast<unsigned>(address);
  return text.str();
}

DeviceInfo describe(libusb_device *device) {
  DeviceInfo info;
  info.bus = libusb_get_bus_number(device);
  info.address = libusb_get_device_address(device);
  info.serial = fallbackSerial(info.bus, info.address);
  libusb_device_descriptor descriptor{};
  if (libusb_get_device_descriptor(device, &descriptor) != 0)
    return info;

  libusb_device_handle *opened = nullptr;
  if (libusb_open(device, &opened) == 0) {
    const auto serial = readString(opened, descriptor.iSerialNumber);
    if (!serial.empty())
      info.serial = serial;
    info.label = readString(opened, descriptor.iProduct);
    libusb_close(opened);
  }
  if (info.label.empty())
    info.label = "Astra918";
  return info;
}

} // namespace

UsbTransport::UsbTransport(void *contextValue, void *handleValue,
                           DeviceInfo info)
    : context_(contextValue), handle_(handleValue), info_(std::move(info)) {}

UsbTransport::~UsbTransport() {
  if (handle_) {
    libusb_release_interface(handle(handle_), kVendorInterface);
    libusb_close(handle(handle_));
  }
  if (context_)
    libusb_exit(context(context_));
}

std::vector<DeviceInfo> UsbTransport::enumerate() {
  libusb_context *ctx = nullptr;
  const int initResult = libusb_init(&ctx);
  if (initResult != 0)
    throw std::runtime_error(usbError("libusb_init", initResult));
  libusb_device **devices = nullptr;
  const auto count = libusb_get_device_list(ctx, &devices);
  if (count < 0) {
    libusb_exit(ctx);
    throw std::runtime_error(
        usbError("libusb_get_device_list", static_cast<int>(count)));
  }
  std::vector<DeviceInfo> found;
  for (auto i = 0; i < count; ++i) {
    libusb_device_descriptor descriptor{};
    if (libusb_get_device_descriptor(devices[i], &descriptor) != 0 ||
        descriptor.idVendor != kVendorId || descriptor.idProduct != kProductId)
      continue;
    found.push_back(describe(devices[i]));
  }
  libusb_free_device_list(devices, 1);
  libusb_exit(ctx);
  return found;
}

std::unique_ptr<UsbTransport> UsbTransport::open(const std::string &serial) {
  libusb_context *ctx = nullptr;
  const int initResult = libusb_init(&ctx);
  if (initResult != 0)
    throw std::runtime_error(usbError("libusb_init", initResult));
  libusb_device **devices = nullptr;
  const auto count = libusb_get_device_list(ctx, &devices);
  if (count < 0) {
    libusb_exit(ctx);
    throw std::runtime_error(
        usbError("libusb_get_device_list", static_cast<int>(count)));
  }

  std::unique_ptr<UsbTransport> result;
  std::string lastOpenError;
  for (auto i = 0; i < count; ++i) {
    libusb_device_descriptor descriptor{};
    if (libusb_get_device_descriptor(devices[i], &descriptor) != 0 ||
        descriptor.idVendor != kVendorId || descriptor.idProduct != kProductId)
      continue;
    const auto info = describe(devices[i]);
    if (info.serial != serial)
      continue;

    libusb_device_handle *opened = nullptr;
    const int openResult = libusb_open(devices[i], &opened);
    if (openResult != 0) {
      lastOpenError =
          usbError("Could not open Astra918 USB device", openResult);
      continue;
    }
    const int claimResult = libusb_claim_interface(opened, kVendorInterface);
    if (claimResult != 0) {
      lastOpenError =
          usbError("Could not claim Astra918 vendor interface 4", claimResult);
      libusb_close(opened);
      continue;
    }
    result.reset(new UsbTransport(ctx, opened, info));
    ctx = nullptr;
    break;
  }
  libusb_free_device_list(devices, 1);
  if (ctx)
    libusb_exit(ctx);
  if (!result) {
    if (!lastOpenError.empty())
      throw std::runtime_error(lastOpenError);
    throw std::runtime_error(
        "Astra918 receiver disappeared; rescan and select it again");
  }
  return result;
}

Bytes UsbTransport::command(const Command commandId, const Bytes &payload) {
  std::lock_guard<std::mutex> lock(commandMutex_);
  if (!controlUsable_)
    throw std::runtime_error(
        "Astra918 control stream lost synchronization; reconnect the receiver");
  const auto sequence = ++sequence_;
  const auto request = makeRecord(commandId, sequence, payload);
  int transferred = 0;
  int result = libusb_bulk_transfer(handle(handle_), kCommandEndpoint,
                                    const_cast<unsigned char *>(request.data()),
                                    static_cast<int>(request.size()),
                                    &transferred, 1000);
  if (result != 0) {
    controlUsable_ = false;
    throw std::runtime_error(usbError("Astra918 control write", result));
  }
  if (transferred != static_cast<int>(request.size())) {
    controlUsable_ = false;
    throw std::runtime_error(
        "Short Astra918 control write; reconnect before retrying");
  }

  Record reply{};
  std::size_t received = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (received < reply.size()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      controlUsable_ = false;
      throw std::runtime_error(
          "Astra918 control reply timeout; reconnect before retrying");
    }
    transferred = 0;
    result = libusb_bulk_transfer(
        handle(handle_), kControlEndpoint, reply.data() + received,
        static_cast<int>(reply.size() - received), &transferred, 100);
    if (result != 0 && result != LIBUSB_ERROR_TIMEOUT) {
      controlUsable_ = false;
      throw std::runtime_error(usbError("Astra918 control read", result));
    }
    if (transferred > 0)
      received += static_cast<std::size_t>(transferred);
  }
  try {
    return parseReply(reply, commandId, sequence);
  } catch (const ProtocolError &error) {
    if (error.status() == 0)
      controlUsable_ = false;
    throw;
  }
}

std::size_t UsbTransport::readIq(std::uint8_t *buffer,
                                 const std::size_t capacity,
                                 const unsigned timeoutMs) {
  if (capacity == 0)
    return 0;
  const auto limited =
      std::min<std::size_t>(capacity, static_cast<std::size_t>(INT32_MAX));
  int transferred = 0;
  const int result =
      libusb_bulk_transfer(handle(handle_), kIqEndpoint, buffer,
                           static_cast<int>(limited), &transferred, timeoutMs);
  if (result != 0 && result != LIBUSB_ERROR_TIMEOUT)
    throw std::runtime_error(usbError("Astra918 I/Q read", result));
  return static_cast<std::size_t>(std::max(transferred, 0));
}

void UsbTransport::drainIq() {
  std::array<std::uint8_t, 16384> buffer{};
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (std::chrono::steady_clock::now() < deadline) {
    if (readIq(buffer.data(), buffer.size(), 30) == 0)
      return;
  }
  throw std::runtime_error(
      "Astra918 I/Q endpoint did not become quiet after stop");
}

} // namespace astra918
