#include "astra918/protocol.hpp"
#include "gqrx_sync.hpp"
#include "soapy_device.hpp"

#include <SoapySDR/Constants.h>
#include <SoapySDR/Errors.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using astra918::Bytes;

void require(const bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}

void putU16(std::uint8_t *out, const std::uint16_t value) {
  out[0] = static_cast<std::uint8_t>(value);
  out[1] = static_cast<std::uint8_t>(value >> 8);
}

void putU32(std::uint8_t *out, const std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i)
    out[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

void putU64(std::uint8_t *out, const std::uint64_t value) {
  putU32(out, static_cast<std::uint32_t>(value));
  putU32(out + 4, static_cast<std::uint32_t>(value >> 32));
}

Bytes statusBytes(const astra918::ReceiverStatus &status) {
  Bytes bytes(128, 0);
  putU64(bytes.data(), status.dialHz);
  putU32(bytes.data() + 16, status.sampleRate);
  putU32(bytes.data() + 20, status.bandwidthHz);
  bytes[28] = status.requestedInput;
  bytes[29] = status.resolvedInput;
  bytes[30] = status.rfGainMode;
  bytes[31] = status.ifGainMode;
  bytes[32] = status.rfGainCode;
  bytes[33] = status.ifGainCode;
  bytes[34] = status.iqEnabled;
  bytes[35] = status.configured;
  putU32(bytes.data() + 36, status.generation);
  putU32(bytes.data() + 40, status.iqDrops);
  putU32(bytes.data() + 44, status.captureFaults);
  putU32(bytes.data() + 48, status.usbFaults);
  putU32(bytes.data() + 52, status.lastError);
  putU16(bytes.data() + 58, static_cast<std::uint16_t>(status.rssiRaw));
  putU16(bytes.data() + 76, status.capacitor);
  bytes[78] = status.lfGainCode;
  bytes[79] = status.lfAttenuatorCode;
  putU64(bytes.data() + 80, status.centerHz);
  putU32(bytes.data() + 88, static_cast<std::uint32_t>(status.audioOffsetHz));
  bytes[92] = status.audioMode;
  putU16(bytes.data() + 94, status.audioLowHz);
  putU32(bytes.data() + 96, status.settingsRevision);
  putU32(bytes.data() + 112, status.savedRevision);
  putU16(bytes.data() + 116, status.audioHighHz);
  bytes[118] = status.referenceSource;
  bytes[119] = status.gpioValues;
  bytes[120] = status.features;
  return bytes;
}

Bytes makeIqFrame(const std::uint32_t generation, const std::uint32_t sequence,
                  const std::uint64_t firstSample, const std::int16_t iValue,
                  const std::int16_t qValue) {
  Bytes frame(astra918::kIqFrameBytes, 0);
  std::memcpy(frame.data(), "ASIQ", 4);
  frame[4] = 1;
  frame[5] = 0x80;
  putU16(frame.data() + 6, 64);
  putU32(frame.data() + 8, generation);
  putU32(frame.data() + 12, sequence);
  putU64(frame.data() + 16, firstSample);
  putU32(frame.data() + 24, astra918::kSampleRate);
  putU16(frame.data() + 28, 512);
  putU64(frame.data() + 32, 14'200'000);
  putU64(frame.data() + 40, 14'201'250);
  putU32(frame.data() + 48, 2);
  for (std::size_t n = 0; n < 512; ++n) {
    putU16(frame.data() + 64 + 4 * n, static_cast<std::uint16_t>(iValue));
    putU16(frame.data() + 66 + 4 * n, static_cast<std::uint16_t>(qValue));
  }
  return frame;
}

class MockTransport final : public astra918::Transport {
public:
  explicit MockTransport(const std::uint8_t features = 0xc0,
                         const bool sequenceGap = false,
                         const bool stopIqOnGain = false,
                         const std::size_t framesPerStream = 1,
                         const std::size_t maxReadChunk = 37)
      : features_(features), sequenceGap_(sequenceGap),
        stopIqOnGain_(stopIqOnGain), framesPerStream_(framesPerStream),
        maxReadChunk_(maxReadChunk) {
    status_.centerHz = 14'200'000;
    status_.audioOffsetHz = 1250;
    status_.dialHz = status_.centerHz + status_.audioOffsetHz;
    status_.sampleRate = astra918::kSampleRate;
    status_.bandwidthHz = 100000;
    status_.requestedInput = 2;
    status_.resolvedInput = 2;
    status_.rfGainCode = 38;
    status_.ifGainCode = 31;
    status_.lfGainCode = 15;
    status_.lfAttenuatorCode = 15;
    status_.audioMode = 2;
    status_.audioLowHz = 100;
    status_.audioHighHz = 3500;
    status_.configured = true;
    status_.features = features_;
  }

  Bytes command(const astra918::Command command,
                const Bytes &payload = {}) override {
    std::lock_guard<std::mutex> lock(mutex_);
    using astra918::Command;
    switch (command) {
    case Command::Capabilities: {
      Bytes caps(204, 0);
      putU32(caps.data(), 2);
      putU32(caps.data() + 4, 70000);
      putU32(caps.data() + 8, 130000000);
      putU32(caps.data() + 12, 100);
      caps[28] = 0x0f;
      caps[29] = 39;
      caps[30] = 32;
      caps[31] = 0x2b;
      putU32(caps.data() + 32, astra918::kSampleRate);
      caps[198] = 0x0f;
      caps[199] = 1;
      putU32(caps.data() + 194, 40);
      putU32(caps.data() + 200, 512);
      return caps;
    }
    case Command::Status:
      return statusBytes(status_);
    case Command::SetDial: {
      require(payload.size() == 8, "SET_DIAL payload size");
      status_.dialHz = astra918::readU64(payload.data());
      status_.centerHz = static_cast<std::uint64_t>(
          static_cast<std::int64_t>(status_.dialHz) - status_.audioOffsetHz);
      ++status_.generation;
      ++status_.settingsRevision;
      return statusBytes(status_);
    }
    case Command::SetAudioOffsetAndDial: {
      require(payload.size() == 8, "SET_OFFSET payload size");
      status_.dialHz = astra918::readU64(payload.data());
      status_.audioOffsetHz = static_cast<std::int32_t>(
          static_cast<std::int64_t>(status_.dialHz) -
          static_cast<std::int64_t>(status_.centerHz));
      ++status_.generation;
      ++status_.settingsRevision;
      return statusBytes(status_);
    }
    case Command::SetInput:
      status_.requestedInput = payload.at(0);
      if (status_.requestedInput != 0)
        status_.resolvedInput = status_.requestedInput;
      ++status_.settingsRevision;
      return statusBytes(status_);
    case Command::SetGainMode:
      if (payload.at(0) == 0)
        status_.rfGainMode = payload.at(1);
      else
        status_.ifGainMode = payload.at(1);
      ++status_.settingsRevision;
      return statusBytes(status_);
    case Command::SetGain:
      if (payload.at(0) == 0)
        status_.rfGainCode = payload.at(1);
      else if (payload.at(0) == 1)
        status_.ifGainCode = payload.at(1);
      else if (payload.at(0) == 2)
        status_.lfGainCode = payload.at(1);
      else
        status_.lfAttenuatorCode = payload.at(1);
      ++status_.settingsRevision;
      if (stopIqOnGain_) {
        status_.iqEnabled = false;
        iqBytes_.clear();
        iqOffset_ = 0;
        ++status_.usbFaults;
      }
      return statusBytes(status_);
    case Command::SetCapacitor:
      status_.capacitor = astra918::readU16(payload.data());
      ++status_.settingsRevision;
      return statusBytes(status_);
    case Command::SetAudioMode:
      status_.audioMode = payload.at(0);
      ++status_.settingsRevision;
      return statusBytes(status_);
    case Command::SetAudioFilter:
      status_.audioLowHz = astra918::readU16(payload.data());
      status_.audioHighHz = astra918::readU16(payload.data() + 2);
      ++status_.settingsRevision;
      return statusBytes(status_);
    case Command::SetReference:
      if ((features_ & 0x40) == 0)
        throw astra918::ProtocolError("unsupported", 5);
      status_.referenceSource = payload.at(0);
      ++status_.settingsRevision;
      return statusBytes(status_);
    case Command::SetGpio: {
      if ((features_ & 0x80) == 0)
        throw astra918::ProtocolError("unsupported", 5);
      const auto mask = payload.at(0);
      status_.gpioValues = static_cast<std::uint8_t>(
          (status_.gpioValues & ~mask) | (payload.at(1) & mask));
      ++status_.settingsRevision;
      return statusBytes(status_);
    }
    case Command::Save:
      ++saveCount;
      status_.savedRevision = status_.settingsRevision;
      return statusBytes(status_);
    case Command::Retry:
    case Command::GetRate:
      return statusBytes(status_);
    case Command::StartIq:
      ++startIqCount;
      status_.iqEnabled = true;
      ++status_.generation;
      iqBytes_.clear();
      for (std::size_t i = 0; i < framesPerStream_; ++i) {
        const auto frame =
            makeIqFrame(status_.generation, static_cast<std::uint32_t>(i),
                        i * astra918::kIqSamplesPerFrame, 16384, -8192);
        iqBytes_.insert(iqBytes_.end(), frame.begin(), frame.end());
      }
      if (sequenceGap_) {
        const auto second =
            makeIqFrame(status_.generation, 2, 1024, -4000, 5000);
        iqBytes_.insert(iqBytes_.end(), second.begin(), second.end());
      }
      iqOffset_ = 0;
      return statusBytes(status_);
    case Command::StopIq:
      status_.iqEnabled = false;
      return statusBytes(status_);
    }
    throw std::runtime_error("Unexpected mock command");
  }

  std::size_t readIq(std::uint8_t *buffer, const std::size_t capacity,
                     const unsigned) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (iqOffset_ >= iqBytes_.size())
      return 0;
    const auto count =
        std::min({capacity, maxReadChunk_, iqBytes_.size() - iqOffset_});
    std::memcpy(buffer, iqBytes_.data() + iqOffset_, count);
    iqOffset_ += count;
    iqBytesRead += count;
    return count;
  }

  void drainIq() override {
    std::lock_guard<std::mutex> lock(mutex_);
    iqOffset_ = iqBytes_.size();
  }

  void tuneExternally(const std::uint64_t center) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.centerHz = center;
    status_.dialHz = center + static_cast<std::int64_t>(status_.audioOffsetHz);
    ++status_.generation;
    if (status_.iqEnabled) {
      iqBytes_ = makeIqFrame(status_.generation, 0, 0, 16384, -8192);
      iqOffset_ = 0;
    }
  }

  astra918::ReceiverStatus status_;
  int saveCount = 0;
  std::atomic<int> startIqCount{0};
  std::atomic<std::size_t> iqBytesRead{0};

private:
  std::mutex mutex_;
  std::uint8_t features_;
  bool sequenceGap_;
  bool stopIqOnGain_;
  std::size_t framesPerStream_;
  std::size_t maxReadChunk_;
  Bytes iqBytes_;
  std::size_t iqOffset_ = 0;
};

void testControlRecords() {
  using namespace astra918;
  const auto request = makeRecord(Command::SetDial, 0x12345678, {1, 2, 3});
  require(std::memcmp(request.data(), "AST1", 4) == 0, "AST1 request magic");
  require(readU32(request.data() + 8) == 0x12345678,
          "AST1 sequence is little endian");
  auto reply = request;
  require(parseReply(reply, Command::SetDial, 0x12345678) == Bytes({1, 2, 3}),
          "AST1 reply payload");
  reply[5] = static_cast<std::uint8_t>(Command::Status);
  bool rejected = false;
  try {
    (void)parseReply(reply, Command::SetDial, 0x12345678);
  } catch (const ProtocolError &) {
    rejected = true;
  }
  require(rejected, "mismatched AST1 command is rejected");

  reply = request;
  reply[6] = 8;
  try {
    (void)parseReply(reply, Command::SetDial, 0x12345678);
    rejected = false;
  } catch (const ProtocolError &error) {
    rejected =
        error.status() == 8 &&
        std::string(error.what()).find("0x20 (status 8)") != std::string::npos;
  }
  require(rejected, "firmware rejections report command and status codes");
}

void testFragmentedIqDecoder() {
  using namespace astra918;
  auto bytes = makeIqFrame(7, 11, 5632, -1234, 2345);
  IqFrameDecoder decoder;
  decoder.setGeneration(7);
  decoder.feed(bytes.data(), 13);
  require(!decoder.pop(), "partial header is retained");
  decoder.feed(bytes.data() + 13, 500);
  require(!decoder.pop(), "partial I/Q payload is retained");
  decoder.feed(bytes.data() + 513, bytes.size() - 513);
  const auto frame = decoder.pop();
  require(frame.has_value(), "complete fragmented I/Q frame is decoded");
  require(frame->sequence == 11 && frame->firstSample == 5632,
          "I/Q position fields decode");
  require(frame->interleavedIq[0] == -1234 && frame->interleavedIq[1] == 2345,
          "signed little-endian I/Q samples decode");

  auto stale = makeIqFrame(6, 12, 6144, 0, 0);
  decoder.feed(stale.data(), stale.size());
  require(!decoder.pop(), "stale generation frame is discarded");
}

void testStatusValidation() {
  auto status = astra918::ReceiverStatus{};
  status.centerHz = 14'200'000;
  status.audioOffsetHz = -1250;
  status.dialHz = 14'198'750;
  status.sampleRate = astra918::kSampleRate;
  status.bandwidthHz = 100000;
  status.requestedInput = 2;
  status.resolvedInput = 2;
  status.rfGainCode = 38;
  status.ifGainCode = 31;
  status.audioMode = 2;
  status.audioLowHz = 100;
  status.audioHighHz = 3500;
  (void)astra918::ReceiverStatus::decode(statusBytes(status));
  status.audioHighHz = 100;
  bool rejected = false;
  try {
    (void)astra918::ReceiverStatus::decode(statusBytes(status));
  } catch (const astra918::ProtocolError &) {
    rejected = true;
  }
  require(rejected, "invalid status passband is rejected");
}

void testLiveFrequencyAndOffset() {
  auto fake = std::make_unique<MockTransport>();
  auto *mock = fake.get();
  astra918::SoapyAstra918 device(std::move(fake), {"MOCK-1", "Astra918", 1, 2},
                                 false);
  device.setFrequency(SOAPY_SDR_RX, 0, 14'300'000.0);
  require(mock->status_.centerHz == 14'300'000,
          "center tune changes spectrum center");
  require(mock->status_.audioOffsetHz == 1250,
          "center tune preserves firmware audio offset");
  require(mock->status_.dialHz == 14'301'250,
          "center tune sends dial including firmware offset");

  mock->tuneExternally(14'400'000);
  require(device.getFrequency(SOAPY_SDR_RX, 0) == 14'400'000.0,
          "frequency readback observes a later external CAT tune");

  device.writeSetting("audio_offset_hz", "-2500");
  require(mock->status_.centerHz == 14'400'000,
          "offset update preserves center");
  require(mock->status_.audioOffsetHz == -2500 &&
              mock->status_.dialHz == 14'397'500,
          "offset update uses the atomic channel-tune command");
  const auto frequencyRange = device.getFrequencyRange(SOAPY_SDR_RX, 0).front();
  require(frequencyRange.minimum() == 72500.0,
          "reported center range accounts for a negative firmware offset");
  require(
      frequencyRange.maximum() == 260000000.0,
      "reported range retains the receiver's operational upper selector bound");
  bool invalid = false;
  try {
    device.setFrequency(SOAPY_SDR_RX, 0, 261'000'000.0);
  } catch (const std::invalid_argument &) {
    invalid = true;
  }
  require(invalid, "out-of-range tuning is rejected");
}

void testFixedBandwidthCompatibility() {
  auto fake = std::make_unique<MockTransport>();
  astra918::SoapyAstra918 device(std::move(fake), {"MOCK-BW", "Astra918", 1, 7},
                                 false);
  device.setBandwidth(SOAPY_SDR_RX, 0, 0.0);
  const auto fixedBandwidth = device.getBandwidth(SOAPY_SDR_RX, 0);
  device.setBandwidth(SOAPY_SDR_RX, 0, fixedBandwidth);

  bool unsupported = false;
  try {
    device.setBandwidth(SOAPY_SDR_RX, 0, fixedBandwidth + 1000.0);
  } catch (const std::invalid_argument &) {
    unsupported = true;
  }
  require(unsupported,
          "a nonzero request to change fixed firmware bandwidth is rejected");

  bool nonFinite = false;
  try {
    device.setBandwidth(SOAPY_SDR_RX, 0,
                        std::numeric_limits<double>::quiet_NaN());
  } catch (const std::invalid_argument &) {
    nonFinite = true;
  }
  require(nonFinite, "non-finite bandwidth requests are rejected");
}

void testBandSpecificGainExposure() {
  auto fake = std::make_unique<MockTransport>();
  auto *mock = fake.get();
  astra918::SoapyAstra918 device(std::move(fake),
                                 {"MOCK-GAIN", "Astra918", 1, 8}, false);
  const auto hasSetting = [](const SoapySDR::ArgInfoList &settings,
                             const std::string &key) {
    return std::any_of(
        settings.begin(), settings.end(),
        [&key](const auto &setting) { return setting.key == key; });
  };

  require(device.listGains(SOAPY_SDR_RX, 0) ==
              std::vector<std::string>({"RF", "IF", "LF", "ATT"}),
          "gain controls remain stable before Gqrx restores its antenna");
  require(!hasSetting(device.getSettingInfo(), "lf_gain_code") &&
              !hasSetting(device.getSettingInfo(), "lf_attenuator_code"),
          "HF input omits unsupported LF gain settings");
  require(device.getGainRange(SOAPY_SDR_RX, 0, "LF").minimum() == 3.5 &&
              device.getGain(SOAPY_SDR_RX, 0, "LF") == 36.4,
          "stored LF gain remains readable while HF is active");
  device.setGain(SOAPY_SDR_RX, 0, "LF", 20.3);
  device.setGain(SOAPY_SDR_RX, 0, "ATT", -10.2);
  require(mock->status_.lfGainCode == 15 &&
              mock->status_.lfAttenuatorCode == 15,
          "saved LF values are ignored while HF is active");

  device.setAntenna(SOAPY_SDR_RX, 0, "LF");
  require(device.listGains(SOAPY_SDR_RX, 0) ==
              std::vector<std::string>({"RF", "IF", "LF", "ATT"}),
          "LF input exposes LF gain and attenuation controls");
  require(hasSetting(device.getSettingInfo(), "lf_gain_code") &&
              hasSetting(device.getSettingInfo(), "lf_attenuator_code"),
          "LF input exposes its LF gain settings");
  device.setGain(SOAPY_SDR_RX, 0, "LF", 20.3);
  device.setGain(SOAPY_SDR_RX, 0, "ATT", -10.2);
  require(mock->status_.lfGainCode == 7 && mock->status_.lfAttenuatorCode == 8,
          "LF gain controls map to firmware codes while LF input is active");
}

void testSettingsAndExplicitSave() {
  auto fake = std::make_unique<MockTransport>();
  auto *mock = fake.get();
  astra918::SoapyAstra918 device(std::move(fake), {"MOCK-2", "Astra918", 1, 3},
                                 false);
  device.setAntenna(SOAPY_SDR_RX, 0, "VHF");
  device.writeSetting("rf_gain_mode", "manual");
  device.writeSetting("rf_gain_code", "20");
  device.writeSetting("lf_mf_capacitor", "2048");
  device.setClockSource("external");
  device.writeSetting("gpio3", "true");
  require(mock->saveCount == 0, "immediate settings do not persist flash");
  require(device.readSetting("gpio3") == "true",
          "logical GPIO setting reads back");
  require(device.readSetting("reference_clock") == "external",
          "clock source reads back");
  require(device.getAntenna(SOAPY_SDR_RX, 0) == "VHF",
          "antenna selection reads back");
  device.writeSetting("save", "true");
  require(mock->saveCount == 1,
          "explicit Save triggers flash persistence once");

  device.setGain(SOAPY_SDR_RX, 0, "IF", 12.0);
  require(
      mock->status_.ifGainMode == 1 && mock->status_.ifGainCode == 14,
      "Soapy gain API selects the nearest documented IF code and manual mode");
}

void testFirmwareFeatureGates() {
  auto fake = std::make_unique<MockTransport>(0);
  astra918::SoapyAstra918 device(std::move(fake), {"OLD", "Astra918", 1, 4},
                                 false);
  require(device.listClockSources() == std::vector<std::string>{"internal"},
          "old firmware does not advertise external reference selection");
  require(device.listGPIOBanks().empty(),
          "old firmware does not advertise logical GPIO");
  bool clockRejected = false;
  try {
    device.setClockSource("external");
  } catch (const std::runtime_error &) {
    clockRejected = true;
  }
  require(clockRejected, "external clock setting is feature-gated");
  bool gpioRejected = false;
  try {
    device.writeSetting("gpio0", "true");
  } catch (const std::runtime_error &) {
    gpioRejected = true;
  }
  require(gpioRejected, "logical GPIO setting is feature-gated");
}

void testSoapyStreamAndConversion() {
  auto fake = std::make_unique<MockTransport>();
  astra918::SoapyAstra918 device(std::move(fake),
                                 {"MOCK-STREAM", "Astra918", 1, 5}, false);
  auto *stream = device.setupStream(SOAPY_SDR_RX, "CF32", {0});
  require(device.getStreamMTU(stream) == 512,
          "Soapy MTU follows the firmware frame size");
  require(device.activateStream(stream) == 0, "mock I/Q stream starts");

  std::array<std::complex<float>, 13> first{};
  void *firstBuffers[] = {first.data()};
  int flags = 0;
  long long timeNs = 0;
  const int firstCount = device.readStream(stream, firstBuffers, first.size(),
                                           flags, timeNs, 100000);
  require(firstCount == static_cast<int>(first.size()),
          "first partial stream read returns requested samples");
  require(std::abs(first[0].real() - 0.5f) < 1e-6f &&
              std::abs(first[0].imag() + 0.25f) < 1e-6f,
          "CF32 samples use signed 16-bit full-scale normalization");

  std::array<std::complex<float>, 9> second{};
  void *secondBuffers[] = {second.data()};
  const int secondCount = device.readStream(
      stream, secondBuffers, second.size(), flags, timeNs, 100000);
  require(secondCount == static_cast<int>(second.size()),
          "remaining frame samples survive short reads");
  require(device.deactivateStream(stream) == 0,
          "mock I/Q stream stops cleanly");
  device.closeStream(stream);
}

void testCs16AndSequenceOverflow() {
  auto fake = std::make_unique<MockTransport>(0xc0, true);
  astra918::SoapyAstra918 device(std::move(fake),
                                 {"MOCK-I16", "Astra918", 1, 6}, false);
  auto *stream = device.setupStream(SOAPY_SDR_RX, "CS16", {0});
  require(device.activateStream(stream) == 0, "CS16 mock stream starts");
  std::array<std::int16_t, 1024> first{};
  void *firstBuffers[] = {first.data()};
  int flags = 0;
  long long timeNs = 0;
  const auto firstCount =
      device.readStream(stream, firstBuffers, 512, flags, timeNs, 200000);
  require(firstCount == 512, "CS16 stream returns a complete firmware frame: " +
                                 std::to_string(firstCount));
  require(first[0] == 16384 && first[1] == -8192,
          "CS16 stream preserves signed native samples");
  require(device.readStream(stream, firstBuffers, 4, flags, timeNs, 200000) ==
              SOAPY_SDR_OVERFLOW,
          "sequence loss is reported as SoapySDR overflow");
  require(device.readStream(stream, firstBuffers, 4, flags, timeNs, 200000) ==
              4,
          "samples following reported loss remain readable");
  require(first[0] == -4000 && first[1] == 5000,
          "post-overflow frame data is preserved");
  device.deactivateStream(stream);
  device.closeStream(stream);
}

void testGainChangeResumesStoppedIq() {
  auto fake = std::make_unique<MockTransport>(0xc0, false, true);
  auto *transport = fake.get();
  astra918::SoapyAstra918 device(std::move(fake),
                                 {"MOCK-RESUME", "Astra918", 1, 7}, false);
  auto *stream = device.setupStream(SOAPY_SDR_RX, "CS16", {0});
  require(device.activateStream(stream) == 0, "initial I/Q activation");
  require(transport->startIqCount == 1, "initial I/Q command sent once");

  device.setGain(SOAPY_SDR_RX, 0, "RF", 30.5);
  std::array<std::int16_t, 1024> samples{};
  void *buffers[] = {samples.data()};
  int flags = 0;
  long long timeNs = 0;
  require(device.readStream(stream, buffers, 512, flags, timeNs, 1000) ==
              SOAPY_SDR_TIMEOUT,
          "first read detects the interrupted I/Q stream");
  require(transport->startIqCount == 2, "stopped I/Q restarts automatically");
  const auto resumedCount =
      device.readStream(stream, buffers, 512, flags, timeNs, 100000);
  require(resumedCount == 512, "samples return after automatic restart");

  device.deactivateStream(stream);
  require(device.readStream(stream, buffers, 512, flags, timeNs, 1000) ==
              SOAPY_SDR_TIMEOUT,
          "inactive Soapy stream stays stopped");
  require(transport->startIqCount == 2,
          "inactive stream is not restarted automatically");
  device.closeStream(stream);
}

void testSustainedBulkIqReads() {
  constexpr std::size_t frameCount = 32;
  auto fake =
      std::make_unique<MockTransport>(0xc0, false, false, frameCount, 16384);
  auto *transport = fake.get();
  astra918::SoapyAstra918 device(std::move(fake),
                                 {"MOCK-BULK", "Astra918", 1, 8}, false);
  auto *stream = device.setupStream(SOAPY_SDR_RX, "CS16", {0});
  require(device.activateStream(stream) == 0, "bulk I/Q stream starts");
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (transport->iqBytesRead.load() < frameCount * astra918::kIqFrameBytes &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  require(transport->iqBytesRead.load() == frameCount * astra918::kIqFrameBytes,
          "USB is drained while the stream consumer is paused");

  std::array<std::int16_t, 8192> samples{};
  void *buffers[] = {samples.data()};
  int flags = 0;
  long long timeNs = 0;
  std::size_t received = 0;
  while (received < frameCount * astra918::kIqSamplesPerFrame) {
    const auto count =
        device.readStream(stream, buffers, 4096, flags, timeNs, 100000);
    require(count > 0, "bulk USB reads retain every I/Q frame");
    received += static_cast<std::size_t>(count);
  }
  require(received == frameCount * astra918::kIqSamplesPerFrame,
          "bulk I/Q stream returns all samples without overflow");
  device.deactivateStream(stream);
  device.closeStream(stream);
}

void testGenerationChangeContinuesStream() {
  auto fake = std::make_unique<MockTransport>();
  auto *transport = fake.get();
  astra918::SoapyAstra918 device(std::move(fake),
                                 {"MOCK-EPOCH", "Astra918", 1, 9}, false);
  auto *stream = device.setupStream(SOAPY_SDR_RX, "CS16", {0});
  require(device.activateStream(stream) == 0, "initial I/Q epoch starts");

  std::array<std::int16_t, 1024> samples{};
  void *buffers[] = {samples.data()};
  int flags = 0;
  long long timeNs = 0;
  require(device.readStream(stream, buffers, 512, flags, timeNs, 100000) == 512,
          "initial epoch supplies samples");

  transport->tuneExternally(14'250'000);
  require(device.getFrequency(SOAPY_SDR_RX, 0) == 14'250'000,
          "receiver generation change is observed");
  require(device.readStream(stream, buffers, 512, flags, timeNs, 100000) == 512,
          "first frame of a new generation does not report a false overflow");
  device.deactivateStream(stream);
  device.closeStream(stream);
}

void testGqrxCenterPlan() {
  // Model Gqrx 2.17.7's RemoteControl::setNewRemoteFreq for a 120 kS/s
  // input. A single F command often moves only its demodulator offset.
  auto verify = [](const std::uint64_t target, const std::int64_t oldFrequency,
                   const std::int64_t oldOffset) {
    auto frequency = oldFrequency;
    auto offset = oldOffset;
    std::int64_t hardwareCenter = frequency - offset;
    const auto plan = astra918::planGqrxCenter(target);
    for (const auto requested : std::array<std::uint64_t, 3>{
             plan.resetHz, plan.centerCommandHz, plan.finalHz}) {
      const auto next = static_cast<std::int64_t>(requested);
      offset += next - frequency;
      constexpr std::int64_t usableHalfBand = 43200;
      if (!((offset > 0 && offset < usableHalfBand) ||
            (offset < 0 && offset > -usableHalfBand))) {
        offset = offset < 0 ? -8640 : 8640;
        hardwareCenter = next - offset;
      }
      frequency = next;
    }
    require(hardwareCenter == static_cast<std::int64_t>(target),
            "Gqrx RF center follows the external receiver tune");
    require(std::abs(frequency - hardwareCenter) == 1,
            "Gqrx demodulator remains within one hertz of center");
  };

  for (const auto target :
       {137000ULL, 7078000ULL, 14200000ULL, 130000000ULL, 259000000ULL}) {
    for (const auto oldFrequency : {14200000LL, 7078000LL, 136000LL}) {
      verify(target, oldFrequency, 0);
      verify(target, oldFrequency, 12000);
      verify(target, oldFrequency, -12000);
    }
  }
  bool rejected = false;
  try {
    astra918::planGqrxCenter(70000);
  } catch (const std::out_of_range &) {
    rejected = true;
  }
  require(rejected, "unsafe Gqrx edge tuning is rejected");
}

void run(const char *name, void (*test)()) {
  test();
  std::cout << "PASS " << name << '\n';
}

} // namespace

int main() {
  try {
    run("control records", testControlRecords);
    run("fragmented I/Q decoder", testFragmentedIqDecoder);
    run("status validation", testStatusValidation);
    run("live frequency and offset", testLiveFrequencyAndOffset);
    run("fixed bandwidth compatibility", testFixedBandwidthCompatibility);
    run("band-specific gain exposure", testBandSpecificGainExposure);
    run("settings and explicit save", testSettingsAndExplicitSave);
    run("firmware feature gates", testFirmwareFeatureGates);
    run("Soapy stream conversion", testSoapyStreamAndConversion);
    run("CS16 and sequence overflow", testCs16AndSequenceOverflow);
    run("gain change resumes stopped I/Q", testGainChangeResumesStoppedIq);
    run("sustained bulk I/Q reads", testSustainedBulkIqReads);
    run("generation change continues I/Q", testGenerationChangeContinuesStream);
    run("Gqrx external frequency plan", testGqrxCenterPlan);
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
  return 0;
}
