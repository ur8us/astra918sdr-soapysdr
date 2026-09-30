#pragma once

#include "usb_transport.hpp"

#include <SoapySDR/Device.hpp>

#include <atomic>
#include <chrono>
#include <complex>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace astra918 {

class SoapyAstra918 final : public SoapySDR::Device {
public:
  SoapyAstra918(std::unique_ptr<Transport> transport, DeviceInfo info,
                bool startPoller = true);
  ~SoapyAstra918() override;

  std::string getDriverKey() const override;
  std::string getHardwareKey() const override;
  SoapySDR::Kwargs getHardwareInfo() const override;
  std::size_t getNumChannels(const int direction) const override;
  SoapySDR::Kwargs getChannelInfo(const int direction,
                                  const std::size_t channel) const override;
  bool getFullDuplex(const int direction,
                     const std::size_t channel) const override;

  std::vector<std::string>
  getStreamFormats(const int direction,
                   const std::size_t channel) const override;
  std::string getNativeStreamFormat(const int direction,
                                    const std::size_t channel,
                                    double &fullScale) const override;
  SoapySDR::Stream *setupStream(const int direction, const std::string &format,
                                const std::vector<std::size_t> &channels = {},
                                const SoapySDR::Kwargs &args = {}) override;
  void closeStream(SoapySDR::Stream *stream) override;
  std::size_t getStreamMTU(SoapySDR::Stream *stream) const override;
  int activateStream(SoapySDR::Stream *stream, const int flags = 0,
                     const long long timeNs = 0,
                     const std::size_t numElems = 0) override;
  int deactivateStream(SoapySDR::Stream *stream, const int flags = 0,
                       const long long timeNs = 0) override;
  int readStream(SoapySDR::Stream *stream, void *const *buffs,
                 const std::size_t numElems, int &flags, long long &timeNs,
                 const long timeoutUs = 100000) override;

  std::vector<std::string>
  listAntennas(const int direction, const std::size_t channel) const override;
  void setAntenna(const int direction, const std::size_t channel,
                  const std::string &name) override;
  std::string getAntenna(const int direction,
                         const std::size_t channel) const override;

  std::vector<std::string> listGains(const int direction,
                                     const std::size_t channel) const override;
  bool hasGainMode(const int direction,
                   const std::size_t channel) const override;
  void setGainMode(const int direction, const std::size_t channel,
                   const bool automatic) override;
  bool getGainMode(const int direction,
                   const std::size_t channel) const override;
  void setGain(const int direction, const std::size_t channel,
               const double value) override;
  void setGain(const int direction, const std::size_t channel,
               const std::string &name, const double value) override;
  double getGain(const int direction, const std::size_t channel) const override;
  double getGain(const int direction, const std::size_t channel,
                 const std::string &name) const override;
  SoapySDR::Range getGainRange(const int direction,
                               const std::size_t channel) const override;
  SoapySDR::Range getGainRange(const int direction, const std::size_t channel,
                               const std::string &name) const override;

  void setFrequency(const int direction, const std::size_t channel,
                    const double frequency,
                    const SoapySDR::Kwargs &args = {}) override;
  void setFrequency(const int direction, const std::size_t channel,
                    const std::string &name, const double frequency,
                    const SoapySDR::Kwargs &args = {}) override;
  double getFrequency(const int direction,
                      const std::size_t channel) const override;
  double getFrequency(const int direction, const std::size_t channel,
                      const std::string &name) const override;
  std::vector<std::string>
  listFrequencies(const int direction,
                  const std::size_t channel) const override;
  SoapySDR::RangeList
  getFrequencyRange(const int direction,
                    const std::size_t channel) const override;
  SoapySDR::RangeList getFrequencyRange(const int direction,
                                        const std::size_t channel,
                                        const std::string &name) const override;

  void setSampleRate(const int direction, const std::size_t channel,
                     const double rate) override;
  double getSampleRate(const int direction,
                       const std::size_t channel) const override;
  std::vector<double> listSampleRates(const int direction,
                                      const std::size_t channel) const override;
  SoapySDR::RangeList
  getSampleRateRange(const int direction,
                     const std::size_t channel) const override;
  void setBandwidth(const int direction, const std::size_t channel,
                    const double bandwidth) override;
  double getBandwidth(const int direction,
                      const std::size_t channel) const override;
  std::vector<double> listBandwidths(const int direction,
                                     const std::size_t channel) const override;
  SoapySDR::RangeList
  getBandwidthRange(const int direction,
                    const std::size_t channel) const override;

  std::vector<std::string> listClockSources() const override;
  void setClockSource(const std::string &source) override;
  std::string getClockSource() const override;

  SoapySDR::ArgInfoList getSettingInfo() const override;
  void writeSetting(const std::string &key, const std::string &value) override;
  std::string readSetting(const std::string &key) const override;
  SoapySDR::ArgInfoList
  getSettingInfo(const int direction, const std::size_t channel) const override;
  void writeSetting(const int direction, const std::size_t channel,
                    const std::string &key, const std::string &value) override;
  std::string readSetting(const int direction, const std::size_t channel,
                          const std::string &key) const override;

  std::vector<std::string> listSensors() const override;
  SoapySDR::ArgInfo getSensorInfo(const std::string &key) const override;
  std::string readSensor(const std::string &key) const override;
  std::vector<std::string>
  listSensors(const int direction, const std::size_t channel) const override;
  SoapySDR::ArgInfo getSensorInfo(const int direction,
                                  const std::size_t channel,
                                  const std::string &key) const override;
  std::string readSensor(const int direction, const std::size_t channel,
                         const std::string &key) const override;

  std::vector<std::string> listGPIOBanks() const override;
  void writeGPIO(const std::string &bank, const unsigned value) override;
  void writeGPIO(const std::string &bank, const unsigned value,
                 const unsigned mask) override;
  unsigned readGPIO(const std::string &bank) const override;
  void writeGPIODir(const std::string &bank, const unsigned dir) override;
  void writeGPIODir(const std::string &bank, const unsigned dir,
                    const unsigned mask) override;
  unsigned readGPIODir(const std::string &bank) const override;

private:
  struct RxStream;
  void checkChannel(int direction, std::size_t channel) const;
  void checkDeviceSettingChannel(int direction, std::size_t channel) const;
  ReceiverStatus cachedStatus() const;
  ReceiverStatus refreshStatus() const;
  Bytes transact(Command command, const Bytes &payload = {}) const;
  ReceiverStatus transactForStatus(Command command, const Bytes &payload = {},
                                   bool updateDecoder = true) const;
  void storeStatus(const ReceiverStatus &status) const;
  void updateDecoderGeneration(std::uint32_t generation) const;
  void pollStatus();
  void captureIq(RxStream *stream);
  SoapySDR::Stream *streamHandle(RxStream *stream) const;
  RxStream &requireStream(SoapySDR::Stream *stream) const;
  void requireFeature(bool supported, const std::string &feature) const;
  bool hasLfGainControls() const;
  void requireLfGainInput() const;
  void writeAudioFilter(std::uint16_t low, std::uint16_t high);

  std::unique_ptr<Transport> transport_;
  DeviceInfo info_;
  bool manualLfRfSupported_ = false;
  bool capacitorSupported_ = false;
  mutable std::mutex controlMutex_;
  mutable std::mutex statusMutex_;
  mutable ReceiverStatus status_;
  mutable std::atomic<bool> haveLatestGeneration_{false};
  mutable std::atomic<std::uint32_t> latestGeneration_{0};
  std::atomic<bool> stopPoller_{false};
  std::thread poller_;
  mutable std::mutex streamMutex_;
  std::unique_ptr<RxStream> rxStream_;
  std::atomic<bool> stoppingIq_{false};
};

SoapySDR::KwargsList findAstra918(const SoapySDR::Kwargs &args);
SoapySDR::Device *makeAstra918(const SoapySDR::Kwargs &args);

} // namespace astra918
