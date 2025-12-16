cmake_minimum_required(VERSION 3.12)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2 CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(pico_watering_system C CXX ASM)

pico_sdk_init()

add_subdirectory(lib/pico-ds3231/src)
include_directories(lib/pico-ds3231/src)

add_subdirectory(lib/pico-epd-driver/src/common)
include_directories(lib/pico-epd-driver/src/common)

add_subdirectory(lib/pico-epd-driver/src/epd_2_13)
include_directories(lib/pico-epd-driver/src/epd_2_13)

add_subdirectory(lib/pico-epd-driver/src/canvas)
include_directories(lib/pico-epd-driver/src/canvas)

add_executable(${CMAKE_PROJECT_NAME} main.c)

pico_enable_stdio_usb(${CMAKE_PROJECT_NAME} 1)
pico_enable_stdio_uart(${CMAKE_PROJECT_NAME} 1)

pico_add_extra_outputs(${CMAKE_PROJECT_NAME})

target_link_libraries(${CMAKE_PROJECT_NAME} pico_stdlib ds3231 epd_2_13 canvas common hardware_i2c hardware_spi hardware_pio hardware_adc hardware_interp hardware_adc hardware_xosc hardware_powman hardware_sync)
