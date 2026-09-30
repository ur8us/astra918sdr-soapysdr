#include "soapy_device.hpp"

#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Version.h>

namespace {
SoapySDR::Registry registerAstra918("astra918", &astra918::findAstra918,
                                    &astra918::makeAstra918,
                                    SOAPY_SDR_ABI_VERSION);
}
