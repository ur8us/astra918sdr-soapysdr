cmake_minimum_required(VERSION 3.16)
project(SoapyAstra918 VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SoapySDR CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

add_library(astra918_core STATIC
    src/protocol.cpp
    src/usb_transport.cpp
    src/soapy_device.cpp)
set_target_properties(astra918_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(astra918_core PUBLIC include PRIVATE src)
target_link_libraries(astra918_core PUBLIC SoapySDR PkgConfig::LIBUSB)
target_compile_options(astra918_core PRIVATE -Wall -Wextra -Wpedantic)

SOAPY_SDR_MODULE_UTIL(
    TARGET SoapyAstra918Support
    SOURCES src/module.cpp
    LIBRARIES astra918_core
    VERSION ${PROJECT_VERSION})
target_include_directories(SoapyAstra918Support PRIVATE include src)

include(CTest)
if(BUILD_TESTING)
    add_executable(astra918_tests tests/astra918_tests.cpp)
    target_include_directories(astra918_tests PRIVATE include src)
    target_link_libraries(astra918_tests PRIVATE astra918_core)
    target_compile_options(astra918_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME astra918_tests COMMAND astra918_tests)
endif()
