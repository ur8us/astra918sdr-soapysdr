#include "soapy_device.hpp"
#include "gqrx_sync.hpp"

#include <SoapySDR/Constants.h>
#include <SoapySDR/Errors.h>
#include <SoapySDR/Logger.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>

namespace astra918 {
namespace {

constexpr std::uint64_t kMinimumFrequencyHz = 70000;
constexpr std::uint64_t kMaximumFrequencyHz = 260000000;

const std::array<double, 39> kRfGainDb = {
    -8.9, -7.9, -6.5, -5.6, -4.6, -3.7, -2.6, -1.6, -0.5, 0.6,
    1.6,  2.5,  3.6,  4.5,  5.6,  6.6,  7.6,  8.7,  9.6,  10.8,
    12.0, 13.2, 14.4, 15.5, 16.8, 17.9, 18.9, 20.2, 21.3, 22.6,
    23.6, 24.6, 25.5, 26.5, 27.5, 28.5, 29.6, 30.5, 31.6};
const std::array<double, 32> kIfGainDb = {
    -1.6, -0.6, 0.4,  1.4,  2.4,  3.4,  4.4,  5.4,  6.4,  7.4,  8.4,
    9.4,  10.4, 11.4, 12.4, 13.4, 14.4, 15.4, 16.4, 17.4, 18.4, 19.4,
    20.4, 21.4, 22.4, 23.4, 24.4, 25.4, 26.4, 27.4, 28.4, 29.4};
const std::array<double, 16> kLfGainDb = {3.5,  6.0,  8.4,  10.8, 13.2, 15.7,
                                          18.0, 20.3, 22.7, 25.0, 27.3, 29.5,
                                          31.7, 33.9, 35.2, 36.4};
const std::array<double, 16> kAttenuationDb = {
    -20.7, -19.3, -17.8, -16.5, -15.0, -13.7, -12.3, -11.0,
    -10.2, -8.8,  -7.4,  -6.0,  -4.6,  -3.2,  -1.7,  0.0};

SoapySDR::ArgInfo arg(const std::string &key, const std::string &name,
                      const std::string &description,
                      const SoapySDR::ArgInfo::Type type,
                      const std::string &value) {
  SoapySDR::ArgInfo info;
  info.key = key;
  info.name = name;
  info.description = description;
  info.type = type;
  info.value = value;
  return info;
}

SoapySDR::ArgInfo intArg(const std::string &key, const std::string &name,
                         const std::string &description,
                         const std::string &value, const long long minimum,
                         const long long maximum, const long long step = 1) {
  auto info = arg(key, name, description, SoapySDR::ArgInfo::INT, value);
  info.range =
      SoapySDR::Range(static_cast<double>(minimum),
                      static_cast<double>(maximum), static_cast<double>(step));
  return info;
}

SoapySDR::ArgInfo choiceArg(const std::string &key, const std::string &name,
                            const std::string &description,
                            const std::string &value,
                            const std::vector<std::string> &values,
                            const std::vector<std::string> &names) {
  auto info = arg(key, name, description, SoapySDR::ArgInfo::STRING, value);
  info.options = values;
  info.optionNames = names;
  return info;
}

template <typename Integer>
bool parseInteger(const std::string &value, Integer &parsed) {
  const char *first = value.data();
  const char *last = first + value.size();
  while (first != last && (*first == ' ' || *first == '\t' || *first == '\n' ||
                           *first == '\v' || *first == '\f' || *first == '\r'))
    ++first;
  if (first != last && *first == '+')
    ++first;
  const auto result = std::from_chars(first, last, parsed, 10);
  return result.ec == std::errc{} && result.ptr == last;
}

std::uint64_t parseUnsigned(const std::string &value,
                            const std::uint64_t maximum,
                            const std::string &key) {
  unsigned long long parsed = 0;
  if (!parseInteger(value, parsed))
    throw std::invalid_argument("Invalid integer value for setting " + key);
  if (parsed > maximum)
    throw std::invalid_argument("Setting " + key +
                                " is outside its supported range");
  return static_cast<std::uint64_t>(parsed);
}

std::int32_t parseSigned32(const std::string &value, const std::string &key) {
  long long parsed = 0;
  if (!parseInteger(value, parsed))
    throw std::invalid_argument("Invalid integer value for setting " + key);
  if (parsed < -60000 || parsed > 60000)
    throw std::invalid_argument("Setting " + key +
                                " is outside its supported range");
  return static_cast<std::int32_t>(parsed);
}

bool parseBool(const std::string &value, const std::string &key) {
  if (value == "true" || value == "1" || value == "on")
    return true;
  if (value == "false" || value == "0" || value == "off")
    return false;
  throw std::invalid_argument("Setting " + key + " expects true or false");
}

std::uint8_t inputCode(const std::string &name) {
  if (name == "Auto")
    return 0;
  if (name == "LF")
    return 1;
  if (name == "HF")
    return 2;
  if (name == "VHF")
    return 3;
  throw std::invalid_argument("RF input must be Auto, LF, HF, or VHF");
}

std::string inputName(const std::uint8_t code) {
  static const std::array<const char *, 4> names = {"Auto", "LF", "HF", "VHF"};
  if (code >= names.size())
    throw std::runtime_error("Receiver reported an invalid RF input");
  return names[code];
}

double tableValue(const std::array<double, 39> &table,
                  const std::uint8_t code) {
  return table.at(code);
}
double tableValue(const std::array<double, 32> &table,
                  const std::uint8_t code) {
  return table.at(code);
}
double tableValue(const std::array<double, 16> &table,
                  const std::uint8_t code) {
  return table.at(code);
}

template <std::size_t N>
std::uint8_t nearestCode(const std::array<double, N> &table,
                         const double value) {
  if (!std::isfinite(value))
    throw std::invalid_argument("Gain must be a finite dB value");
  auto best = std::size_t{0};
  auto distance = std::abs(table[0] - value);
  for (std::size_t i = 1; i < table.size(); ++i) {
    const auto candidate = std::abs(table[i] - value);
    if (candidate < distance) {
      best = i;
      distance = candidate;
    }
  }
  return static_cast<std::uint8_t>(best);
}

std::vector<std::uint8_t> gainPayload(const std::uint8_t block,
                                      const std::uint8_t code) {
  return {block, code};
}

void checkNamedComponent(const std::string &name) {
  if (name != "RF")
    throw std::invalid_argument(
        "Astra918 exposes its spectrum center as the RF frequency");
}

std::uint64_t centerToDial(const std::uint64_t center,
                           const std::int32_t offset) {
  const auto signedDial =
      static_cast<std::int64_t>(center) + static_cast<std::int64_t>(offset);
  if (signedDial <= 0)
    throw std::invalid_argument(
        "Frequency and firmware audio offset produce an invalid dial");
  return static_cast<std::uint64_t>(signedDial);
}

} // namespace

struct SoapyAstra918::RxStream {
  enum class Format { CS16, CF32 } format = Format::CS16;
  std::atomic<bool> active{false};
  IqFrameDecoder decoder;
  std::vector<std::int16_t> pending;
  std::size_t pendingOffset = 0;
  std::optional<std::uint32_t> expectedSequence;
  std::optional<std::uint64_t> expectedFirstSample;
  std::optional<std::uint32_t> decoderGeneration;
  bool overflowPending = false;
  std::uint64_t observedMalformedFrames = 0;
  std::mutex queueMutex;
  std::condition_variable queueReady;
  std::deque<Bytes> iqChunks;
  std::size_t queuedBytes = 0;
  bool queueOverflow = false;
  std::thread captureThread;
};

SoapyAstra918::SoapyAstra918(std::unique_ptr<Transport> transport,
                             DeviceInfo info, const bool startPoller,
                             const std::uint16_t gqrxRemotePort)
    : transport_(std::move(transport)), info_(std::move(info)),
      gqrxRemotePort_(gqrxRemotePort) {
  if (!transport_)
    throw std::invalid_argument("Astra918 USB transport is required");
  const auto capabilities = transact(Command::Capabilities);
  if (capabilities.size() != 204 || readU32(capabilities.data()) != 2 ||
      capabilities[29] != 39 || capabilities[30] != 32 ||
      (capabilities[31] & 0x02u) == 0 ||
      readU32(capabilities.data() + 32) != kSampleRate ||
      readU32(capabilities.data() + 200) != 512)
    throw std::runtime_error("Astra918 firmware does not advertise the "
                             "expected 120 kHz I/Q interface");
  capacitorSupported_ = (capabilities[31] & 0x08u) != 0;
  manualLfRfSupported_ = (capabilities[31] & 0x20u) != 0;
  lastGqrxCenter_.store(refreshStatus().centerHz);
  if (startPoller)
    poller_ = std::thread(&SoapyAstra918::pollStatus, this);
}

SoapyAstra918::~SoapyAstra918() {
  stopPoller_.store(true);
  if (poller_.joinable())
    poller_.join();
  if (rxStream_) {
    try {
      deactivateStream(streamHandle(rxStream_.get()));
    } catch (...) {
    }
    rxStream_.reset();
  }
}

std::string SoapyAstra918::getDriverKey() const { return "astra918"; }
std::string SoapyAstra918::getHardwareKey() const { return "Astra918"; }

SoapySDR::Kwargs SoapyAstra918::getHardwareInfo() const {
  const auto status = cachedStatus();
  return {{"manufacturer", "Astra918 contributors"},
          {"product", info_.label},
          {"serial", info_.serial},
          {"sample_rate", std::to_string(status.sampleRate)},
          {"spectrum_center_hz", std::to_string(status.centerHz)},
          {"audio_offset_hz", std::to_string(status.audioOffsetHz)},
          {"reference_clock_setting",
           status.hasReferenceSelection() ? "supported" : "unsupported"},
          {"vfo_if_setting",
           status.hasVfoIfSelection() ? "supported" : "unsupported"},
          {"logical_gpio_setting",
           status.hasLogicalGpio() ? "supported" : "unsupported"}};
}

std::size_t SoapyAstra918::getNumChannels(const int direction) const {
  if (direction == SOAPY_SDR_RX)
    return 1;
  if (direction == SOAPY_SDR_TX)
    return 0;
  throw std::invalid_argument("Unknown Astra918 channel direction");
}

SoapySDR::Kwargs
SoapyAstra918::getChannelInfo(const int direction,
                              const std::size_t channel) const {
  checkChannel(direction, channel);
  return {{"label", "120 kHz I/Q"}};
}

bool SoapyAstra918::getFullDuplex(const int direction,
                                  const std::size_t channel) const {
  checkChannel(direction, channel);
  return false;
}

std::vector<std::string>
SoapyAstra918::getStreamFormats(const int direction,
                                const std::size_t channel) const {
  checkChannel(direction, channel);
  return {"CS16", "CF32"};
}

std::string SoapyAstra918::getNativeStreamFormat(const int direction,
                                                 const std::size_t channel,
                                                 double &fullScale) const {
  checkChannel(direction, channel);
  fullScale = 32768.0;
  return "CS16";
}

SoapySDR::Stream *SoapyAstra918::streamHandle(RxStream *stream) const {
  return reinterpret_cast<SoapySDR::Stream *>(stream);
}

SoapyAstra918::RxStream &
SoapyAstra918::requireStream(SoapySDR::Stream *stream) const {
  if (!rxStream_ || stream != streamHandle(rxStream_.get()))
    throw std::invalid_argument("Unknown Astra918 stream handle");
  return *rxStream_;
}

SoapySDR::Stream *
SoapyAstra918::setupStream(const int direction, const std::string &format,
                           const std::vector<std::size_t> &channels,
                           const SoapySDR::Kwargs &args) {
  if (direction != SOAPY_SDR_RX)
    throw std::invalid_argument("Astra918 is receive-only");
  if (!channels.empty() && (channels.size() != 1 || channels[0] != 0))
    throw std::invalid_argument("Astra918 has only RX channel 0");
  if (!args.empty())
    throw std::invalid_argument("Astra918 does not define stream arguments");
  auto stream = std::make_unique<RxStream>();
  if (format == "CS16")
    stream->format = RxStream::Format::CS16;
  else if (format == "CF32")
    stream->format = RxStream::Format::CF32;
  else
    throw std::invalid_argument("Astra918 stream format must be CS16 or CF32");
  std::lock_guard<std::mutex> lock(streamMutex_);
  if (rxStream_)
    throw std::runtime_error("Astra918 supports one RX stream at a time");
  auto *raw = stream.get();
  rxStream_ = std::move(stream);
  return streamHandle(raw);
}

void SoapyAstra918::closeStream(SoapySDR::Stream *stream) {
  auto &rx = requireStream(stream);
  if (rx.active.load())
    deactivateStream(stream);
  std::lock_guard<std::mutex> lock(streamMutex_);
  rxStream_.reset();
}

std::size_t SoapyAstra918::getStreamMTU(SoapySDR::Stream *stream) const {
  requireStream(stream);
  return kIqSamplesPerFrame;
}

int SoapyAstra918::activateStream(SoapySDR::Stream *stream, const int flags,
                                  const long long timeNs,
                                  const std::size_t numElems) {
  if (flags != 0 || timeNs != 0 || numElems != 0)
    return SOAPY_SDR_NOT_SUPPORTED;
  auto &rx = requireStream(stream);
  stoppingIq_.store(true);
  std::lock_guard<std::mutex> lock(streamMutex_);
  if (rx.active.load()) {
    stoppingIq_.store(false);
    return 0;
  }
  try {
    transactForStatus(Command::StopIq, {}, false);
    transport_->drainIq();
    rx.decoder.reset();
    rx.pending.clear();
    rx.pendingOffset = 0;
    rx.expectedSequence.reset();
    rx.expectedFirstSample.reset();
    rx.overflowPending = false;
    rx.observedMalformedFrames = 0;
    const auto status = transactForStatus(Command::StartIq, {}, false);
    rx.decoder.setGeneration(status.generation);
    rx.decoderGeneration = status.generation;
    latestGeneration_.store(status.generation, std::memory_order_release);
    haveLatestGeneration_.store(true, std::memory_order_release);
    {
      std::lock_guard<std::mutex> queueLock(rx.queueMutex);
      rx.iqChunks.clear();
      rx.queuedBytes = 0;
      rx.queueOverflow = false;
    }
    rx.active.store(true);
    stoppingIq_.store(false);
    rx.captureThread = std::thread(&SoapyAstra918::captureIq, this, &rx);
    return 0;
  } catch (...) {
    stoppingIq_.store(false);
    throw;
  }
}

int SoapyAstra918::deactivateStream(SoapySDR::Stream *stream, const int flags,
                                    const long long timeNs) {
  if (flags != 0 || timeNs != 0)
    return SOAPY_SDR_NOT_SUPPORTED;
  auto &rx = requireStream(stream);
  stoppingIq_.store(true);
  std::lock_guard<std::mutex> lock(streamMutex_);
  if (!rx.active.load()) {
    if (rx.captureThread.joinable())
      rx.captureThread.join();
    stoppingIq_.store(false);
    return 0;
  }
  try {
    rx.active.store(false);
    rx.queueReady.notify_all();
    if (rx.captureThread.joinable())
      rx.captureThread.join();
    transactForStatus(Command::StopIq, {}, false);
    transport_->drainIq();
    rx.decoder.reset();
    rx.pending.clear();
    rx.pendingOffset = 0;
    {
      std::lock_guard<std::mutex> queueLock(rx.queueMutex);
      rx.iqChunks.clear();
      rx.queuedBytes = 0;
      rx.queueOverflow = false;
    }
    stoppingIq_.store(false);
    return 0;
  } catch (...) {
    stoppingIq_.store(false);
    throw;
  }
}

void SoapyAstra918::captureIq(RxStream *rx) {
  // The firmware must be drained continuously: its USB write timeout is
  // shorter than a GUI redraw or a synchronous gain-control call can take.
  std::array<std::uint8_t, 4 * kIqFrameBytes> buffer{};
  while (rx->active.load() && !stoppingIq_.load()) {
    std::size_t received = 0;
    try {
      received = transport_->readIq(buffer.data(), buffer.size(), 100);
    } catch (const std::exception &error) {
      SoapySDR::logf(SOAPY_SDR_WARNING, "Astra918 I/Q USB read failed: %s",
                     error.what());
    }
    if (!rx->active.load() || stoppingIq_.load())
      break;
    if (received) {
      {
        std::lock_guard<std::mutex> lock(rx->queueMutex);
        // Keep roughly one second of I/Q so brief application stalls do not
        // back up the receiver. Drop old data if the consumer truly falls
        // behind and report the gap through SoapySDR.
        while (rx->queuedBytes + received > 512 * 1024) {
          rx->queuedBytes -= rx->iqChunks.front().size();
          rx->iqChunks.pop_front();
          rx->queueOverflow = true;
        }
        rx->iqChunks.emplace_back(buffer.begin(), buffer.begin() + received);
        rx->queuedBytes += received;
      }
      rx->queueReady.notify_one();
      continue;
    }

    try {
      const auto status = refreshStatus();
      if (status.configured && !status.iqEnabled && rx->active.load() &&
          !stoppingIq_.load()) {
        const auto resumed = transactForStatus(Command::StartIq, {}, false);
        {
          std::lock_guard<std::mutex> lock(rx->queueMutex);
          rx->iqChunks.clear();
          rx->queuedBytes = 0;
          rx->queueOverflow = false;
        }
        latestGeneration_.store(resumed.generation, std::memory_order_release);
        haveLatestGeneration_.store(true, std::memory_order_release);
        SoapySDR::logf(SOAPY_SDR_INFO,
                       "Astra918 I/Q resumed after firmware USB timeout "
                       "(generation %u)",
                       resumed.generation);
        rx->queueReady.notify_all();
      }
    } catch (const std::exception &error) {
      SoapySDR::logf(SOAPY_SDR_WARNING, "Astra918 I/Q resume failed: %s",
                     error.what());
    }
    // Mock transports can return an immediate empty read; avoid a hot loop.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  rx->queueReady.notify_all();
}

int SoapyAstra918::readStream(SoapySDR::Stream *stream, void *const *buffs,
                              const std::size_t numElems, int &flags,
                              long long &timeNs, const long timeoutUs) {
  flags = 0;
  timeNs = 0;
  if (!buffs || !buffs[0])
    return SOAPY_SDR_STREAM_ERROR;
  RxStream *rxPointer = nullptr;
  try {
    rxPointer = &requireStream(stream);
  } catch (...) {
    return SOAPY_SDR_STREAM_ERROR;
  }
  auto &rx = *rxPointer;
  if (!rx.active.load()) {
    if (timeoutUs > 0)
      std::this_thread::sleep_for(std::chrono::microseconds(timeoutUs));
    return SOAPY_SDR_TIMEOUT;
  }
  if (numElems == 0)
    return 0;

  std::unique_lock<std::mutex> lock(streamMutex_);
  if (rx.overflowPending) {
    rx.overflowPending = false;
    return SOAPY_SDR_OVERFLOW;
  }
  const auto timeout = std::chrono::microseconds(std::max<long>(timeoutUs, 0));
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::size_t copied = 0;

  while (copied < numElems) {
    if (haveLatestGeneration_.load(std::memory_order_acquire)) {
      const auto generation = latestGeneration_.load(std::memory_order_acquire);
      if (!rx.decoderGeneration || *rx.decoderGeneration != generation) {
        if (copied > 0)
          break;
        rx.decoder.reset();
        rx.decoder.setGeneration(generation);
        rx.decoderGeneration = generation;
        rx.pending.clear();
        rx.pendingOffset = 0;
        rx.expectedSequence.reset();
        rx.expectedFirstSample.reset();
        rx.overflowPending = false;
      }
    }
    if (rx.pendingOffset < rx.pending.size() / 2) {
      const auto available = rx.pending.size() / 2 - rx.pendingOffset;
      const auto count = std::min(available, numElems - copied);
      if (rx.format == RxStream::Format::CS16) {
        auto *out = static_cast<std::int16_t *>(buffs[0]) + 2 * copied;
        std::memcpy(out, rx.pending.data() + 2 * rx.pendingOffset,
                    count * 2 * sizeof(std::int16_t));
      } else {
        auto *out = static_cast<std::complex<float> *>(buffs[0]) + copied;
        for (std::size_t i = 0; i < count; ++i) {
          const float real =
              static_cast<float>(rx.pending[2 * (rx.pendingOffset + i)]) /
              32768.0f;
          const float imag =
              static_cast<float>(rx.pending[2 * (rx.pendingOffset + i) + 1]) /
              32768.0f;
          out[i] = {real, imag};
        }
      }
      rx.pendingOffset += count;
      copied += count;
      if (rx.pendingOffset >= rx.pending.size() / 2) {
        rx.pending.clear();
        rx.pendingOffset = 0;
      }
      if (copied == numElems)
        break;
    }

    const auto malformedBefore = rx.decoder.malformedFrames();
    auto frame = rx.decoder.pop();
    if (rx.decoder.malformedFrames() != malformedBefore) {
      rx.observedMalformedFrames = rx.decoder.malformedFrames();
      return copied == 0 ? SOAPY_SDR_CORRUPTION : static_cast<int>(copied);
    }
    if (frame) {
      if (rx.expectedSequence &&
          (*rx.expectedSequence != frame->sequence ||
           *rx.expectedFirstSample != frame->firstSample))
        rx.overflowPending = true;
      rx.expectedSequence = frame->sequence + 1;
      rx.expectedFirstSample = frame->firstSample + kIqSamplesPerFrame;
      rx.pending = std::move(frame->interleavedIq);
      rx.pendingOffset = 0;
      if (rx.overflowPending && copied > 0)
        return static_cast<int>(copied);
      if (rx.overflowPending) {
        rx.overflowPending = false;
        return SOAPY_SDR_OVERFLOW;
      }
      continue;
    }

    if (stoppingIq_.load() || !rx.active.load())
      break;
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
      break;
    Bytes chunk;
    {
      std::unique_lock<std::mutex> queueLock(rx.queueMutex);
      rx.queueReady.wait_until(queueLock, deadline, [&] {
        return !rx.iqChunks.empty() || rx.queueOverflow || !rx.active.load() ||
               stoppingIq_.load();
      });
      if (rx.queueOverflow) {
        rx.queueOverflow = false;
        rx.decoder.reset();
        rx.pending.clear();
        rx.pendingOffset = 0;
        rx.expectedSequence.reset();
        rx.expectedFirstSample.reset();
        if (copied == 0)
          return SOAPY_SDR_OVERFLOW;
        rx.overflowPending = true;
        break;
      }
      if (!rx.iqChunks.empty()) {
        chunk = std::move(rx.iqChunks.front());
        rx.iqChunks.pop_front();
        rx.queuedBytes -= chunk.size();
      }
    }
    if (!chunk.empty())
      rx.decoder.feed(chunk.data(), chunk.size());
  }
  if (copied > 0) {
    return static_cast<int>(copied);
  }
  return SOAPY_SDR_TIMEOUT;
}

void SoapyAstra918::checkChannel(const int direction,
                                 const std::size_t channel) const {
  if (direction != SOAPY_SDR_RX || channel != 0)
    throw std::invalid_argument("Astra918 supports only RX channel 0");
}

void SoapyAstra918::checkDeviceSettingChannel(const int direction,
                                              const std::size_t channel) const {
  checkChannel(direction, channel);
}

Bytes SoapyAstra918::transact(const Command command,
                              const Bytes &payload) const {
  std::lock_guard<std::mutex> lock(controlMutex_);
  return transport_->command(command, payload);
}

void SoapyAstra918::storeStatus(const ReceiverStatus &status) const {
  std::lock_guard<std::mutex> lock(statusMutex_);
  status_ = status;
}

ReceiverStatus
SoapyAstra918::transactForStatus(const Command command, const Bytes &payload,
                                 const bool updateDecoder) const {
  Bytes reply;
  {
    std::lock_guard<std::mutex> lock(controlMutex_);
    reply = transport_->command(command, payload);
    if (reply.size() != 128)
      reply = transport_->command(Command::Status);
    const auto status = ReceiverStatus::decode(reply);
    storeStatus(status);
    if (!updateDecoder)
      return status;
    // Release control serialization before waiting for the I/Q consumer lock.
    // Caller-triggered retunes must not block the USB control endpoint behind a
    // read. The scoped lock is intentionally ended below by returning after
    // this block.
  }
  const auto status = cachedStatus();
  if (updateDecoder)
    updateDecoderGeneration(status.generation);
  return status;
}

ReceiverStatus SoapyAstra918::refreshStatus() const {
  const auto status = transactForStatus(Command::Status);
  return status;
}

ReceiverStatus SoapyAstra918::cachedStatus() const {
  std::lock_guard<std::mutex> lock(statusMutex_);
  return status_;
}

void SoapyAstra918::updateDecoderGeneration(
    const std::uint32_t generation) const {
  latestGeneration_.store(generation, std::memory_order_release);
  haveLatestGeneration_.store(true, std::memory_order_release);
}

void SoapyAstra918::pollStatus() {
  auto nextGqrxAttempt = std::chrono::steady_clock::now();
  std::string lastGqrxError;
  while (!stopPoller_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (stopPoller_.load())
      break;
    try {
      const auto status = refreshStatus();
      if (gqrxRemotePort_ == 0 || status.centerHz == lastGqrxCenter_.load() ||
          std::chrono::steady_clock::now() < nextGqrxAttempt)
        continue;
      nextGqrxAttempt =
          std::chrono::steady_clock::now() + std::chrono::seconds(1);
      gqrxSyncInProgress_.store(true);
      std::string error;
      const bool synced =
          syncGqrxCenter(gqrxRemotePort_, status.centerHz, error);
      gqrxSyncInProgress_.store(false);
      if (synced) {
        lastGqrxCenter_.store(status.centerHz);
        lastGqrxError.clear();
      } else if (error != lastGqrxError) {
        SoapySDR::log(SOAPY_SDR_WARNING,
                      "Astra918 Gqrx frequency sync: " + error);
        lastGqrxError = error;
      }
    } catch (...) {
      gqrxSyncInProgress_.store(false);
      break;
    }
  }
}

void SoapyAstra918::requireFeature(const bool supported,
                                   const std::string &feature) const {
  if (!supported)
    throw std::runtime_error("Connected Astra918 firmware does not support " +
                             feature);
}

void SoapyAstra918::requireLfGainInput() const {
  requireFeature(manualLfRfSupported_, "manual LF gain");
  if (refreshStatus().resolvedInput != 1)
    throw std::invalid_argument(
        "LF gain and attenuation require the LF RF input to be active");
}

std::vector<std::string>
SoapyAstra918::listAntennas(const int direction,
                            const std::size_t channel) const {
  checkChannel(direction, channel);
  return {"Auto", "LF", "HF", "VHF"};
}

void SoapyAstra918::setAntenna(const int direction, const std::size_t channel,
                               const std::string &name) {
  checkChannel(direction, channel);
  const Bytes payload{inputCode(name)};
  transactForStatus(Command::SetInput, payload);
}

std::string SoapyAstra918::getAntenna(const int direction,
                                      const std::size_t channel) const {
  checkChannel(direction, channel);
  return inputName(refreshStatus().requestedInput);
}

std::vector<std::string>
SoapyAstra918::listGains(const int direction, const std::size_t channel) const {
  checkChannel(direction, channel);
  // Gqrx reads the gain names once, before it restores the saved antenna.
  // Keep this list stable across input changes so its controls stay valid.
  if (manualLfRfSupported_)
    return {"RF", "IF", "LF", "ATT"};
  return {"RF", "IF"};
}

bool SoapyAstra918::hasGainMode(const int direction,
                                const std::size_t channel) const {
  checkChannel(direction, channel);
  return true;
}

void SoapyAstra918::setGainMode(const int direction, const std::size_t channel,
                                const bool automatic) {
  checkChannel(direction, channel);
  const auto status = refreshStatus();
  const auto requested = static_cast<std::uint8_t>(automatic ? 0 : 1);
  if (status.rfGainMode != requested)
    transactForStatus(Command::SetGainMode, {0, requested});
}

bool SoapyAstra918::getGainMode(const int direction,
                                const std::size_t channel) const {
  checkChannel(direction, channel);
  return refreshStatus().rfGainMode == 0;
}

void SoapyAstra918::setGain(const int direction, const std::size_t channel,
                            const double value) {
  setGain(direction, channel, "RF", value);
}

void SoapyAstra918::setGain(const int direction, const std::size_t channel,
                            const std::string &name, const double value) {
  checkChannel(direction, channel);
  if (name == "RF") {
    const auto code = nearestCode(kRfGainDb, value);
    const auto status = refreshStatus();
    if (status.rfGainMode != 1)
      transactForStatus(Command::SetGainMode, {0, 1});
    if (status.rfGainCode != code)
      transactForStatus(Command::SetGain, gainPayload(0, code));
  } else if (name == "IF") {
    const auto code = nearestCode(kIfGainDb, value);
    const auto status = refreshStatus();
    if (status.ifGainMode != 1)
      transactForStatus(Command::SetGainMode, {1, 1});
    if (status.ifGainCode != code)
      transactForStatus(Command::SetGain, gainPayload(1, code));
  } else if (name == "LF") {
    requireFeature(manualLfRfSupported_, "manual LF gain");
    const auto code = nearestCode(kLfGainDb, value);
    const auto status = refreshStatus();
    if (status.resolvedInput == 1 && status.lfGainCode != code)
      transactForStatus(Command::SetGain, gainPayload(2, code));
  } else if (name == "ATT") {
    requireFeature(manualLfRfSupported_, "manual LF attenuation");
    const auto code = nearestCode(kAttenuationDb, value);
    const auto status = refreshStatus();
    if (status.resolvedInput == 1 && status.lfAttenuatorCode != code)
      transactForStatus(Command::SetGain, gainPayload(3, code));
  } else {
    throw std::invalid_argument("Gain name must be RF, IF, LF, or ATT");
  }
}

double SoapyAstra918::getGain(const int direction,
                              const std::size_t channel) const {
  return getGain(direction, channel, "RF");
}

double SoapyAstra918::getGain(const int direction, const std::size_t channel,
                              const std::string &name) const {
  checkChannel(direction, channel);
  const auto status = refreshStatus();
  if (name == "RF")
    return tableValue(kRfGainDb, status.rfGainCode);
  if (name == "IF")
    return tableValue(kIfGainDb, status.ifGainCode);
  if (name == "LF") {
    requireFeature(manualLfRfSupported_, "manual LF gain");
    return tableValue(kLfGainDb, status.lfGainCode);
  }
  if (name == "ATT") {
    requireFeature(manualLfRfSupported_, "manual LF attenuation");
    return tableValue(kAttenuationDb, status.lfAttenuatorCode);
  }
  throw std::invalid_argument("Unknown Astra918 gain element");
}

SoapySDR::Range SoapyAstra918::getGainRange(const int direction,
                                            const std::size_t channel) const {
  return getGainRange(direction, channel, "RF");
}

SoapySDR::Range SoapyAstra918::getGainRange(const int direction,
                                            const std::size_t channel,
                                            const std::string &name) const {
  checkChannel(direction, channel);
  if (name == "RF")
    return {-8.9, 31.6, 0.1};
  if (name == "IF")
    return {-1.6, 29.4, 0.1};
  if (name == "LF" && manualLfRfSupported_)
    return {3.5, 36.4, 0.1};
  if (name == "ATT" && manualLfRfSupported_)
    return {-20.7, 0.0, 0.1};
  throw std::invalid_argument("Unknown or unsupported Astra918 gain element");
}

void SoapyAstra918::setFrequency(const int direction, const std::size_t channel,
                                 const double frequency,
                                 const SoapySDR::Kwargs &) {
  checkChannel(direction, channel);
  // Gqrx's remote F command emits intermediate host retunes. The poller is
  // updating Gqrx's display to match firmware; none of these are user tunes.
  if (gqrxSyncInProgress_.load())
    return;
  if (!std::isfinite(frequency) ||
      frequency < static_cast<double>(kMinimumFrequencyHz) ||
      frequency > static_cast<double>(kMaximumFrequencyHz))
    throw std::invalid_argument(
        "Spectrum center is outside the Astra918 tuning range");
  const auto requestedCenter =
      static_cast<std::uint64_t>(std::llround(frequency));
  const auto status = refreshStatus();
  const auto dial = centerToDial(requestedCenter, status.audioOffsetHz);
  if (dial < kMinimumFrequencyHz || dial > kMaximumFrequencyHz)
    throw std::invalid_argument("Spectrum center plus firmware audio offset is "
                                "outside the Astra918 tuning range");
  Bytes payload;
  appendU64(payload, dial);
  const auto applied = transactForStatus(Command::SetDial, payload);
  lastGqrxCenter_.store(applied.centerHz);
}

void SoapyAstra918::setFrequency(const int direction, const std::size_t channel,
                                 const std::string &name,
                                 const double frequency,
                                 const SoapySDR::Kwargs &args) {
  checkNamedComponent(name);
  setFrequency(direction, channel, frequency, args);
}

double SoapyAstra918::getFrequency(const int direction,
                                   const std::size_t channel) const {
  checkChannel(direction, channel);
  return static_cast<double>(refreshStatus().centerHz);
}

double SoapyAstra918::getFrequency(const int direction,
                                   const std::size_t channel,
                                   const std::string &name) const {
  checkNamedComponent(name);
  return getFrequency(direction, channel);
}

std::vector<std::string>
SoapyAstra918::listFrequencies(const int direction,
                               const std::size_t channel) const {
  checkChannel(direction, channel);
  return {"RF"};
}

SoapySDR::RangeList
SoapyAstra918::getFrequencyRange(const int direction,
                                 const std::size_t channel) const {
  checkChannel(direction, channel);
  const auto offset = static_cast<std::int64_t>(refreshStatus().audioOffsetHz);
  const auto minimum = std::max<std::int64_t>(
      static_cast<std::int64_t>(kMinimumFrequencyHz),
      static_cast<std::int64_t>(kMinimumFrequencyHz) - offset);
  const auto maximum = std::min<std::int64_t>(
      static_cast<std::int64_t>(kMaximumFrequencyHz),
      static_cast<std::int64_t>(kMaximumFrequencyHz) - offset);
  return {SoapySDR::Range(static_cast<double>(minimum),
                          static_cast<double>(maximum), 100.0)};
}

SoapySDR::RangeList
SoapyAstra918::getFrequencyRange(const int direction, const std::size_t channel,
                                 const std::string &name) const {
  checkNamedComponent(name);
  return getFrequencyRange(direction, channel);
}

void SoapyAstra918::setSampleRate(const int direction,
                                  const std::size_t channel,
                                  const double rate) {
  checkChannel(direction, channel);
  if (std::abs(rate - static_cast<double>(kSampleRate)) > 0.5)
    throw std::invalid_argument(
        "Astra918 I/Q sample rate is fixed at 120000 samples/s");
}

double SoapyAstra918::getSampleRate(const int direction,
                                    const std::size_t channel) const {
  checkChannel(direction, channel);
  return static_cast<double>(refreshStatus().sampleRate);
}

std::vector<double>
SoapyAstra918::listSampleRates(const int direction,
                               const std::size_t channel) const {
  checkChannel(direction, channel);
  return {static_cast<double>(kSampleRate)};
}

SoapySDR::RangeList
SoapyAstra918::getSampleRateRange(const int direction,
                                  const std::size_t channel) const {
  checkChannel(direction, channel);
  return {SoapySDR::Range(kSampleRate, kSampleRate)};
}

void SoapyAstra918::setBandwidth(const int direction, const std::size_t channel,
                                 const double bandwidth) {
  checkChannel(direction, channel);
  if (!std::isfinite(bandwidth))
    throw std::invalid_argument("Bandwidth must be finite");
  // Gqrx uses zero to mean that no bandwidth was specified. The firmware's
  // wide I/Q bandwidth is fixed, so interpret that value as leave unchanged.
  if (bandwidth == 0.0)
    return;
  const auto actual = refreshStatus().bandwidthHz;
  if (std::abs(bandwidth - static_cast<double>(actual)) > 0.5)
    throw std::invalid_argument(
        "Astra918 wide I/Q filter bandwidth is fixed by its firmware");
}

double SoapyAstra918::getBandwidth(const int direction,
                                   const std::size_t channel) const {
  checkChannel(direction, channel);
  return static_cast<double>(refreshStatus().bandwidthHz);
}

std::vector<double>
SoapyAstra918::listBandwidths(const int direction,
                              const std::size_t channel) const {
  return {getBandwidth(direction, channel)};
}

SoapySDR::RangeList
SoapyAstra918::getBandwidthRange(const int direction,
                                 const std::size_t channel) const {
  const auto bandwidth = getBandwidth(direction, channel);
  return {SoapySDR::Range(bandwidth, bandwidth)};
}

std::vector<std::string> SoapyAstra918::listClockSources() const {
  const auto status = refreshStatus();
  if (status.hasReferenceSelection())
    return {"internal", "external"};
  return {"internal"};
}

void SoapyAstra918::setClockSource(const std::string &source) {
  if (source != "internal" && source != "external")
    throw std::invalid_argument("Clock source must be internal or external");
  const auto status = refreshStatus();
  if (source == "external")
    requireFeature(status.hasReferenceSelection(),
                   "external reference selection");
  const Bytes payload{static_cast<std::uint8_t>(source == "external" ? 1 : 0)};
  try {
    transactForStatus(Command::SetReference, payload);
  } catch (...) {
    try {
      refreshStatus();
    } catch (...) {
    }
    throw;
  }
}

std::string SoapyAstra918::getClockSource() const {
  const auto status = refreshStatus();
  return status.referenceSource == 0 ? "internal" : "external";
}

SoapySDR::ArgInfoList SoapyAstra918::getSettingInfo() const {
  const auto status = cachedStatus();
  SoapySDR::ArgInfoList info;
  info.push_back(choiceArg("rf_input", "RF input", "Requested receiver input.",
                           inputName(status.requestedInput),
                           {"Auto", "LF", "HF", "VHF"},
                           {"Auto", "LF", "HF", "VHF"}));
  info.push_back(choiceArg("rf_gain_mode", "RF gain mode",
                           "RF gain automatic or manual.",
                           status.rfGainMode == 0 ? "auto" : "manual",
                           {"auto", "manual"}, {"Auto", "Manual"}));
  info.push_back(choiceArg("if_gain_mode", "IF gain mode",
                           "IF gain automatic or manual.",
                           status.ifGainMode == 0 ? "auto" : "manual",
                           {"auto", "manual"}, {"Auto", "Manual"}));
  info.push_back(intArg("rf_gain_code", "RF gain code",
                        "Hardware RF gain step (0–38).",
                        std::to_string(status.rfGainCode), 0, 38));
  info.push_back(intArg("if_gain_code", "IF gain code",
                        "Hardware IF gain step (0–31).",
                        std::to_string(status.ifGainCode), 0, 31));
  if (manualLfRfSupported_ && status.resolvedInput == 1) {
    info.push_back(intArg("lf_gain_code", "LF gain code",
                          "Combined LF LNA/mixer gain code.",
                          std::to_string(status.lfGainCode), 0, 15));
    info.push_back(intArg("lf_attenuator_code", "LF attenuator code",
                          "LF attenuator code.",
                          std::to_string(status.lfAttenuatorCode), 0, 15));
  }
  if (capacitorSupported_)
    info.push_back(intArg("lf_mf_capacitor", "LF/MF capacitor",
                          "LF/MF capacitor code (0–4095).",
                          std::to_string(status.capacitor), 0, 4095));
  info.push_back(intArg("audio_offset_hz", "Firmware USB audio offset",
                        "Channel offset from spectrum center.",
                        std::to_string(status.audioOffsetHz), -60000, 60000));
  info.push_back(choiceArg(
      "audio_mode", "Firmware USB audio mode", "USB or LSB demodulator mode.",
      status.audioMode == 2 ? "USB" : "LSB", {"USB", "LSB"}, {"USB", "LSB"}));
  info.push_back(intArg("audio_low_hz", "Audio filter low edge",
                        "Firmware audio passband low edge.",
                        std::to_string(status.audioLowHz), 0, 4999));
  info.push_back(intArg("audio_high_hz", "Audio filter high edge",
                        "Firmware audio passband high edge.",
                        std::to_string(status.audioHighHz), 1, 5000));
  info.push_back(arg("save", "Save to receiver",
                     "Explicitly persist current settings to receiver flash.",
                     SoapySDR::ArgInfo::BOOL, "false"));
  info.push_back(arg("retry", "Retry receiver",
                     "Retry RF configuration after a recoverable fault.",
                     SoapySDR::ArgInfo::BOOL, "false"));
  if (status.hasVfoIfSelection()) {
    info.push_back(choiceArg(
        "vfo_sign", "VFO sign", "Local oscillator position relative to the signal.",
        std::vector<std::string>{"auto", "lo_above", "lo_below"}.at(status.vfoSign),
        {"auto", "lo_above", "lo_below"},
        {"Auto", "LO above the signal", "LO below the signal"}));
    info.push_back(choiceArg(
        "if_frequency", "IF frequency", "Receiver IF selection; Auto uses 96 kHz.",
        std::vector<std::string>{"auto", "96", "120"}.at(status.ifFrequency),
        {"auto", "96", "120"}, {"Auto (96 kHz)", "96 kHz", "120 kHz"}));
  }
  if (status.hasReferenceSelection())
    info.push_back(
        choiceArg("reference_clock", "Reference clock",
                  "Internal or external 38.4 MHz reference.",
                  status.referenceSource == 0 ? "internal" : "external",
                  {"internal", "external"}, {"Internal", "External"}));
  if (status.hasLogicalGpio()) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      const auto key = "gpio" + std::to_string(bit);
      info.push_back(
          arg(key, "GPIO" + std::to_string(bit),
              "Logical stored GPIO value; no physical pin is assigned.",
              SoapySDR::ArgInfo::BOOL,
              (status.gpioValues & (1u << bit)) ? "true" : "false"));
    }
  }
  return info;
}

void SoapyAstra918::writeAudioFilter(const std::uint16_t low,
                                     const std::uint16_t high) {
  if (low >= high || high > 5000)
    throw std::invalid_argument(
        "Audio passband must satisfy 0 <= low < high <= 5000 Hz");
  Bytes payload;
  appendU16(payload, low);
  appendU16(payload, high);
  transactForStatus(Command::SetAudioFilter, payload);
}

void SoapyAstra918::writeSetting(const std::string &key,
                                 const std::string &value) {
  if (key == "rf_input") {
    transactForStatus(Command::SetInput, {inputCode(value)});
  } else if (key == "rf_gain_mode" || key == "if_gain_mode") {
    if (value != "auto" && value != "manual")
      throw std::invalid_argument("Gain mode must be auto or manual");
    const auto block = static_cast<std::uint8_t>(key == "rf_gain_mode" ? 0 : 1);
    const auto mode = static_cast<std::uint8_t>(value == "auto" ? 0 : 1);
    transactForStatus(Command::SetGainMode, {block, mode});
  } else if (key == "rf_gain_code" || key == "if_gain_code" ||
             key == "lf_gain_code" || key == "lf_attenuator_code") {
    std::uint8_t block = 0;
    std::uint64_t maximum = 38;
    if (key == "if_gain_code") {
      block = 1;
      maximum = 31;
    } else if (key == "lf_gain_code") {
      block = 2;
      maximum = 15;
    } else if (key == "lf_attenuator_code") {
      block = 3;
      maximum = 15;
    }
    if (block >= 2)
      requireLfGainInput();
    transactForStatus(
        Command::SetGain,
        gainPayload(block, static_cast<std::uint8_t>(
                               parseUnsigned(value, maximum, key))));
  } else if (key == "lf_mf_capacitor") {
    requireFeature(capacitorSupported_, "LF/MF capacitor tuning");
    Bytes payload;
    appendU16(payload,
              static_cast<std::uint16_t>(parseUnsigned(value, 4095, key)));
    transactForStatus(Command::SetCapacitor, payload);
  } else if (key == "audio_offset_hz") {
    const auto offset = parseSigned32(value, key);
    const auto status = refreshStatus();
    const auto dial = centerToDial(status.centerHz, offset);
    Bytes payload;
    appendU64(payload, dial);
    transactForStatus(Command::SetAudioOffsetAndDial, payload);
  } else if (key == "audio_mode") {
    if (value != "USB" && value != "LSB")
      throw std::invalid_argument("Audio mode must be USB or LSB");
    transactForStatus(Command::SetAudioMode,
                      {static_cast<std::uint8_t>(value == "USB" ? 2 : 1)});
  } else if (key == "audio_low_hz" || key == "audio_high_hz") {
    const auto status = refreshStatus();
    auto low = status.audioLowHz;
    auto high = status.audioHighHz;
    if (key == "audio_low_hz")
      low = static_cast<std::uint16_t>(parseUnsigned(value, 4999, key));
    else
      high = static_cast<std::uint16_t>(parseUnsigned(value, 5000, key));
    writeAudioFilter(low, high);
  } else if (key == "reference_clock") {
    setClockSource(value);
  } else if (key == "vfo_sign" || key == "if_frequency") {
    requireFeature(refreshStatus().hasVfoIfSelection(), "VFO sign/IF selection");
    const std::vector<std::string> choices = key == "vfo_sign"
        ? std::vector<std::string>{"auto", "lo_above", "lo_below"}
        : std::vector<std::string>{"auto", "96", "120"};
    const auto choice = std::find(choices.begin(), choices.end(), value);
    if (choice == choices.end())
      throw std::invalid_argument("Invalid Astra918 " + key + " choice");
    transactForStatus(key == "vfo_sign" ? Command::SetVfoSign : Command::SetIfFrequency,
                      {static_cast<std::uint8_t>(choice - choices.begin())});
  } else if (key == "save") {
    if (parseBool(value, key))
      transactForStatus(Command::Save);
  } else if (key == "retry") {
    if (parseBool(value, key))
      transactForStatus(Command::Retry);
  } else if (key.size() == 5 && key.compare(0, 4, "gpio") == 0 &&
             key[4] >= '0' && key[4] <= '7') {
    const auto status = refreshStatus();
    requireFeature(status.hasLogicalGpio(), "logical GPIO values");
    const auto bit = static_cast<std::uint8_t>(1u << (key[4] - '0'));
    const auto valueBit =
        static_cast<std::uint8_t>(parseBool(value, key) ? bit : 0);
    transactForStatus(Command::SetGpio, {bit, valueBit});
  } else {
    throw std::invalid_argument("Unknown Astra918 setting: " + key);
  }
}

std::string SoapyAstra918::readSetting(const std::string &key) const {
  const auto status = refreshStatus();
  if (key == "rf_input")
    return inputName(status.requestedInput);
  if (key == "rf_gain_mode")
    return status.rfGainMode == 0 ? "auto" : "manual";
  if (key == "if_gain_mode")
    return status.ifGainMode == 0 ? "auto" : "manual";
  if (key == "rf_gain_code")
    return std::to_string(status.rfGainCode);
  if (key == "if_gain_code")
    return std::to_string(status.ifGainCode);
  if (key == "lf_gain_code")
    return std::to_string(status.lfGainCode);
  if (key == "lf_attenuator_code")
    return std::to_string(status.lfAttenuatorCode);
  if (key == "lf_mf_capacitor")
    return std::to_string(status.capacitor);
  if (key == "audio_offset_hz")
    return std::to_string(status.audioOffsetHz);
  if (key == "audio_mode")
    return status.audioMode == 2 ? "USB" : "LSB";
  if (key == "audio_low_hz")
    return std::to_string(status.audioLowHz);
  if (key == "audio_high_hz")
    return std::to_string(status.audioHighHz);
  if (key == "reference_clock")
    return status.referenceSource == 0 ? "internal" : "external";
  if (key == "vfo_sign") {
    requireFeature(status.hasVfoIfSelection(), "VFO sign selection");
    return std::vector<std::string>{"auto", "lo_above", "lo_below"}.at(status.vfoSign);
  }
  if (key == "if_frequency") {
    requireFeature(status.hasVfoIfSelection(), "IF frequency selection");
    return std::vector<std::string>{"auto", "96", "120"}.at(status.ifFrequency);
  }
  if (key == "save" || key == "retry")
    return "false";
  if (key.size() == 5 && key.compare(0, 4, "gpio") == 0 && key[4] >= '0' &&
      key[4] <= '7') {
    requireFeature(status.hasLogicalGpio(), "logical GPIO values");
    const auto bit = static_cast<unsigned>(key[4] - '0');
    return (status.gpioValues & (1u << bit)) ? "true" : "false";
  }
  throw std::invalid_argument("Unknown Astra918 setting: " + key);
}

SoapySDR::ArgInfoList
SoapyAstra918::getSettingInfo(const int direction,
                              const std::size_t channel) const {
  checkDeviceSettingChannel(direction, channel);
  return {};
}

void SoapyAstra918::writeSetting(const int direction, const std::size_t channel,
                                 const std::string &key,
                                 const std::string &value) {
  checkDeviceSettingChannel(direction, channel);
  writeSetting(key, value);
}

std::string SoapyAstra918::readSetting(const int direction,
                                       const std::size_t channel,
                                       const std::string &key) const {
  checkDeviceSettingChannel(direction, channel);
  return readSetting(key);
}

std::vector<std::string> SoapyAstra918::listSensors() const {
  return {"center_frequency_hz", "dial_frequency_hz", "audio_offset_hz",
          "sample_rate",         "iq_drops",          "capture_faults",
          "usb_faults",          "last_error",        "settings_revision",
          "saved_revision",      "rssi_raw"};
}

SoapySDR::ArgInfo SoapyAstra918::getSensorInfo(const std::string &key) const {
  const auto values = listSensors();
  if (std::find(values.begin(), values.end(), key) == values.end())
    throw std::invalid_argument("Unknown Astra918 sensor: " + key);
  const auto type =
      key == "rssi_raw" ? SoapySDR::ArgInfo::FLOAT : SoapySDR::ArgInfo::INT;
  auto info =
      arg(key, key, "Live receiver status read from firmware.", type, "0");
  if (key.find("frequency") != std::string::npos)
    info.units = "Hz";
  if (key == "sample_rate")
    info.units = "S/s";
  return info;
}

std::string SoapyAstra918::readSensor(const std::string &key) const {
  const auto status = refreshStatus();
  if (key == "center_frequency_hz")
    return std::to_string(status.centerHz);
  if (key == "dial_frequency_hz")
    return std::to_string(status.dialHz);
  if (key == "audio_offset_hz")
    return std::to_string(status.audioOffsetHz);
  if (key == "sample_rate")
    return std::to_string(status.sampleRate);
  if (key == "iq_drops")
    return std::to_string(status.iqDrops);
  if (key == "capture_faults")
    return std::to_string(status.captureFaults);
  if (key == "usb_faults")
    return std::to_string(status.usbFaults);
  if (key == "last_error")
    return std::to_string(status.lastError);
  if (key == "settings_revision")
    return std::to_string(status.settingsRevision);
  if (key == "saved_revision")
    return std::to_string(status.savedRevision);
  if (key == "rssi_raw")
    return std::to_string(status.rssiRaw);
  throw std::invalid_argument("Unknown Astra918 sensor: " + key);
}

std::vector<std::string>
SoapyAstra918::listSensors(const int direction,
                           const std::size_t channel) const {
  checkChannel(direction, channel);
  return {};
}

SoapySDR::ArgInfo SoapyAstra918::getSensorInfo(const int direction,
                                               const std::size_t channel,
                                               const std::string &key) const {
  checkChannel(direction, channel);
  (void)key;
  throw std::invalid_argument(
      "Astra918 exposes status sensors at device scope");
}

std::string SoapyAstra918::readSensor(const int direction,
                                      const std::size_t channel,
                                      const std::string &key) const {
  checkChannel(direction, channel);
  (void)key;
  throw std::invalid_argument(
      "Astra918 exposes status sensors at device scope");
}

std::vector<std::string> SoapyAstra918::listGPIOBanks() const {
  return refreshStatus().hasLogicalGpio() ? std::vector<std::string>{"GPIO"}
                                          : std::vector<std::string>{};
}

void SoapyAstra918::writeGPIO(const std::string &bank, const unsigned value) {
  writeGPIO(bank, value, 0xffu);
}

void SoapyAstra918::writeGPIO(const std::string &bank, const unsigned value,
                              const unsigned mask) {
  if (bank != "GPIO")
    throw std::invalid_argument("Unknown Astra918 GPIO bank");
  if ((value & ~0xffu) != 0 || (mask & ~0xffu) != 0)
    throw std::invalid_argument(
        "Astra918 logical GPIO values use only bits 0 through 7");
  const auto status = refreshStatus();
  requireFeature(status.hasLogicalGpio(), "logical GPIO values");
  transactForStatus(Command::SetGpio, {static_cast<std::uint8_t>(mask),
                                       static_cast<std::uint8_t>(value)});
}

unsigned SoapyAstra918::readGPIO(const std::string &bank) const {
  if (bank != "GPIO")
    throw std::invalid_argument("Unknown Astra918 GPIO bank");
  const auto status = refreshStatus();
  requireFeature(status.hasLogicalGpio(), "logical GPIO values");
  return status.gpioValues;
}

void SoapyAstra918::writeGPIODir(const std::string &, const unsigned) {
  throw std::runtime_error("Astra918 GPIO values are logical state only; "
                           "physical pin direction is not assigned");
}

void SoapyAstra918::writeGPIODir(const std::string &, const unsigned,
                                 const unsigned) {
  throw std::runtime_error("Astra918 GPIO values are logical state only; "
                           "physical pin direction is not assigned");
}

unsigned SoapyAstra918::readGPIODir(const std::string &) const {
  throw std::runtime_error("Astra918 GPIO values are logical state only; "
                           "physical pin direction is not assigned");
}

SoapySDR::KwargsList findAstra918(const SoapySDR::Kwargs &args) {
  std::vector<DeviceInfo> devices;
  try {
    devices = UsbTransport::enumerate();
  } catch (...) {
    return {};
  }
  SoapySDR::KwargsList found;
  for (const auto &device : devices) {
    if (const auto serial = args.find("serial");
        serial != args.end() && serial->second != device.serial)
      continue;
    found.push_back({{"driver", "astra918"},
                     {"label", device.label + " (" + device.serial + ")"},
                     {"serial", device.serial},
                     {"bus", std::to_string(device.bus)},
                     {"address", std::to_string(device.address)}});
  }
  return found;
}

SoapySDR::Device *makeAstra918(const SoapySDR::Kwargs &args) {
  std::uint16_t gqrxRemotePort = 0;
  if (const auto it = args.find("gqrx_sync");
      it != args.end() && parseBool(it->second, "gqrx_sync")) {
    gqrxRemotePort = 7356;
    if (const auto port = args.find("gqrx_port"); port != args.end()) {
      const auto value = parseUnsigned(port->second, 65535, "gqrx_port");
      if (value == 0)
        throw std::invalid_argument("gqrx_port must be nonzero");
      gqrxRemotePort = static_cast<std::uint16_t>(value);
    }
  }
  std::string serial;
  if (const auto it = args.find("serial"); it != args.end()) {
    serial = it->second;
  } else {
    const auto devices = UsbTransport::enumerate();
    if (devices.empty())
      throw std::runtime_error("No Astra918 USB receiver found");
    if (devices.size() != 1)
      throw std::runtime_error(
          "More than one Astra918 receiver found; select one by serial");
    serial = devices.front().serial;
  }
  auto transport = UsbTransport::open(serial);
  const auto info = transport->info();
  return new SoapyAstra918(std::move(transport), info, true, gqrxRemotePort);
}

} // namespace astra918
