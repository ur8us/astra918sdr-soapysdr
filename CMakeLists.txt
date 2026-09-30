cmake_minimum_required(VERSION 3.16)
project(SoapyAstra918 VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SoapySDR CONFIG REQUIRED)
# SoapySDR 0.7's package config resets this to C++11; this driver requires C++17.
set(CMAKE_CXX_STANDARD 17)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBUSB QUIET IMPORTED_TARGET libusb-1.0)
endif()
if(TARGET PkgConfig::LIBUSB)
    set(ASTRA918_LIBUSB_TARGET PkgConfig::LIBUSB)
else()
    find_path(LIBUSB_INCLUDE_DIR NAMES libusb.h PATH_SUFFIXES libusb-1.0)
    find_library(LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0)
    if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
        message(FATAL_ERROR "libusb-1.0 development files were not found")
    endif()
    add_library(Astra918LibUSB UNKNOWN IMPORTED)
    set_target_properties(Astra918LibUSB PROPERTIES
        IMPORTED_LOCATION "${LIBUSB_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBUSB_INCLUDE_DIR}")
    set(ASTRA918_LIBUSB_TARGET Astra918LibUSB)
endif()

add_library(astra918_core STATIC
    src/protocol.cpp
    src/usb_transport.cpp
    src/soapy_device.cpp)
set_target_properties(astra918_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(astra918_core PUBLIC include ${SoapySDR_INCLUDE_DIRS} PRIVATE src)
target_link_libraries(astra918_core PUBLIC ${SoapySDR_LIBRARIES} ${ASTRA918_LIBUSB_TARGET})
if(WIN32 AND NOT TARGET PkgConfig::LIBUSB)
    target_link_libraries(astra918_core PUBLIC setupapi cfgmgr32 advapi32)
endif()
if(MSVC)
    target_compile_options(astra918_core PRIVATE /W4)
else()
    target_compile_options(astra918_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

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
    if(MSVC)
        target_compile_options(astra918_tests PRIVATE /W4)
    else()
        target_compile_options(astra918_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME astra918_tests COMMAND astra918_tests)
endif()
