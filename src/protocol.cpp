#include "astra918/protocol.hpp"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace astra918 {

ProtocolError::ProtocolError(const std::string &message,
                             const std::uint8_t status)
    : std::runtime_error(message), status_(status) {}

std::uint16_t readU16(const std::uint8_t *data) {
  return static_cast<std::uint16_t>(data[0]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[1]) << 8);
}

std::uint32_t readU32(const std::uint8_t *data) {
  return static_cast<std::uint32_t>(data[0]) |
         (static_cast<std::uint32_t>(data[1]) << 8) |
         (static_cast<std::uint32_t>(data[2]) << 16) |
         (static_cast<std::uint32_t>(data[3]) << 24);
}

std::uint64_t readU64(const std::uint8_t *data) {
  return static_cast<std::uint64_t>(readU32(data)) |
         (static_cast<std::uint64_t>(readU32(data + 4)) << 32);
}

void appendU16(Bytes &out, const std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void appendU32(Bytes &out, const std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void appendU64(Bytes &out, const std::uint64_t value) {
  appendU32(out, static_cast<std::uint32_t>(value));
  appendU32(out, static_cast<std::uint32_t>(value >> 32));
}

Record makeRecord(const Command command, const std::uint32_t sequence,
                  const Bytes &payload) {
  if (payload.size() > 240)
    throw ProtocolError("AST1 payload exceeds 240 bytes");
  Record record{};
  std::memcpy(record.data(), "AST1", 4);
  record[4] = 1;
  record[5] = static_cast<std::uint8_t>(command);
  record[8] = static_cast<std::uint8_t>(sequence);
  record[9] = static_cast<std::uint8_t>(sequence >> 8);
  record[10] = static_cast<std::uint8_t>(sequence >> 16);
  record[11] = static_cast<std::uint8_t>(sequence >> 24);
  record[12] = static_cast<std::uint8_t>(payload.size());
  record[13] = static_cast<std::uint8_t>(payload.size() >> 8);
  std::copy(payload.begin(), payload.end(), record.begin() + 16);
  return record;
}

Bytes parseReply(const Record &record, const Command command,
                 const std::uint32_t sequence) {
  if (std::memcmp(record.data(), "AST1", 4) != 0 || record[4] != 1 ||
      record[5] != static_cast<std::uint8_t>(command) ||
      readU32(record.data() + 8) != sequence)
    throw ProtocolError("Mismatched Astra918 control reply");
  const auto length = readU16(record.data() + 12);
  if (length > 240 || record[7] != 0 || readU16(record.data() + 14) != 0 ||
      std::any_of(record.begin() + 16 + length, record.end(),
                  [](const auto byte) { return byte != 0; }))
    throw ProtocolError("Malformed Astra918 control reply");
  if (record[6] != 0) {
    std::ostringstream message;
    message << "Astra918 rejected command 0x" << std::hex
            << static_cast<unsigned>(command) << " (status " << std::dec
            << static_cast<unsigned>(record[6]) << ')';
    throw ProtocolError(message.str(), record[6]);
  }
  return Bytes(record.begin() + 16, record.begin() + 16 + length);
}

ReceiverStatus ReceiverStatus::decode(const Bytes &payload) {
  if (payload.size() != 128)
    throw ProtocolError("Invalid Astra918 status length");
  ReceiverStatus status;
  const auto *p = payload.data();
  status.dialHz = readU64(p);
  status.sampleRate = readU32(p + 16);
  status.bandwidthHz = readU32(p + 20);
  status.requestedInput = p[28];
  status.resolvedInput = p[29];
  status.rfGainMode = p[30];
  status.ifGainMode = p[31];
  status.rfGainCode = p[32];
  status.ifGainCode = p[33];
  status.iqEnabled = p[34] != 0;
  status.configured = p[35] != 0;
  status.generation = readU32(p + 36);
  status.iqDrops = readU32(p + 40);
  status.captureFaults = readU32(p + 44);
  status.usbFaults = readU32(p + 48);
  status.lastError = readU32(p + 52);
  status.rssiRaw = static_cast<std::int16_t>(readU16(p + 58));
  status.capacitor = readU16(p + 76);
  status.lfGainCode = p[78];
  status.lfAttenuatorCode = p[79];
  status.centerHz = readU64(p + 80);
  status.audioOffsetHz = static_cast<std::int32_t>(readU32(p + 88));
  status.audioMode = p[92];
  status.audioLowHz = readU16(p + 94);
  status.settingsRevision = readU32(p + 96);
  status.savedRevision = readU32(p + 112);
  status.audioHighHz = readU16(p + 116);
  status.referenceSource = p[118];
  status.gpioValues = p[119];
  status.features = p[120];
  if (status.hasVfoIfSelection()) {
    status.vfoSign = p[121];
    status.ifFrequency = p[122];
  }
  if (status.sampleRate != kSampleRate || status.requestedInput > 3 ||
      status.resolvedInput < 1 || status.resolvedInput > 3 ||
      status.rfGainMode > 1 || status.ifGainMode > 1 ||
      status.rfGainCode > 38 || status.ifGainCode > 31 ||
      status.capacitor > 4095 || status.lfGainCode > 15 ||
      status.lfAttenuatorCode > 15 || status.audioOffsetHz < -60000 ||
      status.audioOffsetHz > 60000 || status.audioMode < 1 ||
      status.audioMode > 2 || status.audioLowHz >= status.audioHighHz ||
      status.audioHighHz > 5000 || status.referenceSource > 1 ||
      status.vfoSign > 2 || status.ifFrequency > 2 ||
      (status.dialHz !=
       status.centerHz + static_cast<std::int64_t>(status.audioOffsetHz)))
    throw ProtocolError("Invalid Astra918 status fields");
  return status;
}

void IqFrameDecoder::reset() {
  buffer_.clear();
  malformedFrames_ = 0;
}

void IqFrameDecoder::setGeneration(
    const std::optional<std::uint32_t> generation) {
  generation_ = generation;
}

void IqFrameDecoder::feed(const std::uint8_t *data, const std::size_t size) {
  if (size == 0)
    return;
  buffer_.insert(buffer_.end(), data, data + size);
  if (buffer_.size() > 8 * kIqFrameBytes) {
    const auto keep = std::min<std::size_t>(buffer_.size(), 3);
    buffer_.erase(buffer_.begin(),
                  buffer_.end() - static_cast<std::ptrdiff_t>(keep));
    ++malformedFrames_;
  }
}

std::optional<IqFrame> IqFrameDecoder::pop() {
  for (;;) {
    const auto marker =
        std::search(buffer_.begin(), buffer_.end(), "ASIQ", "ASIQ" + 4);
    if (marker == buffer_.end()) {
      const auto keep = std::min<std::size_t>(buffer_.size(), 3);
      if (buffer_.size() > keep)
        buffer_.erase(buffer_.begin(),
                      buffer_.end() - static_cast<std::ptrdiff_t>(keep));
      return std::nullopt;
    }
    if (marker != buffer_.begin())
      buffer_.erase(buffer_.begin(), marker);
    if (buffer_.size() < 64)
      return std::nullopt;
    const bool validHeader =
        buffer_[4] == 1 && buffer_[5] == 0x80 &&
        readU16(buffer_.data() + 6) == 64 &&
        readU32(buffer_.data() + 24) == kSampleRate &&
        readU16(buffer_.data() + 28) == kIqSamplesPerFrame &&
        readU16(buffer_.data() + 30) == 0;
    if (!validHeader) {
      buffer_.erase(buffer_.begin());
      ++malformedFrames_;
      continue;
    }
    if (buffer_.size() < kIqFrameBytes)
      return std::nullopt;
    IqFrame frame;
    frame.generation = readU32(buffer_.data() + 8);
    frame.sequence = readU32(buffer_.data() + 12);
    frame.firstSample = readU64(buffer_.data() + 16);
    frame.sampleRate = readU32(buffer_.data() + 24);
    frame.centerHz = readU64(buffer_.data() + 32);
    frame.dialHz = readU64(buffer_.data() + 40);
    frame.settingsRevision = readU32(buffer_.data() + 48);
    if (generation_ && frame.generation != *generation_) {
      buffer_.erase(buffer_.begin(), buffer_.begin() + kIqFrameBytes);
      continue;
    }
    frame.interleavedIq.resize(2 * kIqSamplesPerFrame);
    for (std::size_t i = 0; i < frame.interleavedIq.size(); ++i)
      frame.interleavedIq[i] =
          static_cast<std::int16_t>(readU16(buffer_.data() + 64 + 2 * i));
    buffer_.erase(buffer_.begin(), buffer_.begin() + kIqFrameBytes);
    return frame;
  }
}

} // namespace astra918
