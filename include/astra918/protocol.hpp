#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace astra918 {

using Bytes = std::vector<std::uint8_t>;
using Record = std::array<std::uint8_t, 256>;

constexpr std::uint16_t kVendorId = 0xc0de;
constexpr std::uint16_t kProductId = 0x091a;
constexpr int kVendorInterface = 4;
constexpr unsigned char kCommandEndpoint = 0x03;
constexpr unsigned char kControlEndpoint = 0x84;
constexpr unsigned char kIqEndpoint = 0x85;
constexpr std::size_t kIqFrameBytes = 2112;
constexpr std::size_t kIqSamplesPerFrame = 512;
constexpr std::uint32_t kSampleRate = 120000;

enum class Command : std::uint8_t {
  Capabilities = 0x12,
  Status = 0x13,
  SetDial = 0x20,
  GetRate = 0x23,
  SetInput = 0x24,
  SetGainMode = 0x26,
  SetGain = 0x27,
  SetCapacitor = 0x2d,
  StartIq = 0x30,
  StopIq = 0x31,
  SetAudioOffsetAndDial = 0x38,
  SetReference = 0x3a,
  SetGpio = 0x3b,
  SetVfoSign = 0x3c,
  SetIfFrequency = 0x3d,
  SetAudioMode = 0x34,
  SetAudioFilter = 0x35,
  Save = 0x36,
  Retry = 0x37,
};

class ProtocolError : public std::runtime_error {
public:
  explicit ProtocolError(const std::string &message, std::uint8_t status = 0);
  std::uint8_t status() const noexcept { return status_; }

private:
  std::uint8_t status_;
};

std::uint16_t readU16(const std::uint8_t *data);
std::uint32_t readU32(const std::uint8_t *data);
std::uint64_t readU64(const std::uint8_t *data);
void appendU16(Bytes &out, std::uint16_t value);
void appendU32(Bytes &out, std::uint32_t value);
void appendU64(Bytes &out, std::uint64_t value);

Record makeRecord(Command command, std::uint32_t sequence,
                  const Bytes &payload = {});
Bytes parseReply(const Record &record, Command command, std::uint32_t sequence);

struct ReceiverStatus {
  std::uint64_t dialHz = 0;
  std::uint32_t sampleRate = 0;
  std::uint32_t bandwidthHz = 0;
  std::uint8_t requestedInput = 0;
  std::uint8_t resolvedInput = 0;
  std::uint8_t rfGainMode = 0;
  std::uint8_t ifGainMode = 0;
  std::uint8_t rfGainCode = 0;
  std::uint8_t ifGainCode = 0;
  bool iqEnabled = false;
  bool configured = false;
  std::uint32_t generation = 0;
  std::uint32_t iqDrops = 0;
  std::uint32_t captureFaults = 0;
  std::uint32_t usbFaults = 0;
  std::uint32_t lastError = 0;
  std::int16_t rssiRaw = 0;
  std::uint16_t capacitor = 0;
  std::uint8_t lfGainCode = 0;
  std::uint8_t lfAttenuatorCode = 0;
  std::uint64_t centerHz = 0;
  std::int32_t audioOffsetHz = 0;
  std::uint8_t audioMode = 2;
  std::uint16_t audioLowHz = 100;
  std::uint16_t audioHighHz = 3500;
  std::uint32_t settingsRevision = 0;
  std::uint32_t savedRevision = 0xffffffffu;
  std::uint8_t referenceSource = 0;
  std::uint8_t gpioValues = 0;
  std::uint8_t vfoSign = 0;
  std::uint8_t ifFrequency = 0;
  std::uint8_t features = 0;

  bool hasVfoIfSelection() const { return (features & 0x20u) != 0; }
  bool hasReferenceSelection() const { return (features & 0x40u) != 0; }
  bool hasLogicalGpio() const { return (features & 0x80u) != 0; }
  static ReceiverStatus decode(const Bytes &payload);
};

struct IqFrame {
  std::uint32_t generation = 0;
  std::uint32_t sequence = 0;
  std::uint64_t firstSample = 0;
  std::uint32_t sampleRate = 0;
  std::uint64_t centerHz = 0;
  std::uint64_t dialHz = 0;
  std::uint32_t settingsRevision = 0;
  std::vector<std::int16_t> interleavedIq;
};

class IqFrameDecoder {
public:
  void reset();
  void setGeneration(std::optional<std::uint32_t> generation);
  void feed(const std::uint8_t *data, std::size_t size);
  std::optional<IqFrame> pop();
  std::uint64_t malformedFrames() const { return malformedFrames_; }

private:
  Bytes buffer_;
  std::optional<std::uint32_t> generation_;
  std::uint64_t malformedFrames_ = 0;
};

} // namespace astra918
