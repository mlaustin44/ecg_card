cmake_minimum_required(VERSION 3.20)
project(ecg_card C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Werror -Wshadow -Wdouble-promotion -Wundef)

# ECG front-end gains (board.h): set both to match R35 / R68 on a reworked
# board, e.g. -DECG_GAIN_LO=252 -DECG_GAIN_HI=561. Empty = as built (558 / 1098).
set(ECG_GAIN_LO "" CACHE STRING "ECG gain with GAIN_HI low (empty: board.h default)")
set(ECG_GAIN_HI "" CACHE STRING "ECG gain with GAIN_HI high (empty: board.h default)")
if(NOT ECG_GAIN_LO STREQUAL "" OR NOT ECG_GAIN_HI STREQUAL "")
  if(NOT ECG_GAIN_LO MATCHES "^[0-9]+$" OR NOT ECG_GAIN_HI MATCHES "^[0-9]+$"
     OR NOT ECG_GAIN_LO LESS ECG_GAIN_HI)
    message(FATAL_ERROR "ECG_GAIN_LO and ECG_GAIN_HI must both be set, integers, LO < HI")
  endif()
  add_compile_definitions(ECG_GAIN_LO=${ECG_GAIN_LO} ECG_GAIN_HI=${ECG_GAIN_HI})
  message(STATUS "ECG front-end gains: ${ECG_GAIN_LO} / ${ECG_GAIN_HI}")
endif()

# Portable interface headers: board.h, hal.h, settings.h, ui_model.h, fw_config.h
add_library(ecg_iface INTERFACE)
target_include_directories(ecg_iface INTERFACE ${CMAKE_SOURCE_DIR}/fw/include)

if(CMAKE_CROSSCOMPILING)
  set(ECG_TARGET ON)
else()
  set(ECG_TARGET OFF)
  option(ECG_SANITIZE "Build host code with ASan/UBSan" ON)
  if(ECG_SANITIZE)
    # Some distributions ship GCC without the sanitizer runtimes; probe first.
    include(CheckCSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
    check_c_source_compiles("int main(void){return 0;}" ECG_HAVE_SANITIZERS)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(ECG_HAVE_SANITIZERS)
      add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
      add_link_options(-fsanitize=address,undefined)
    else()
      message(WARNING "Sanitizer runtimes not found for ${CMAKE_C_COMPILER}; "
                      "building without ASan/UBSan (try -DCMAKE_C_COMPILER=clang)")
    endif()
  endif()
  enable_testing()
  add_subdirectory(host/testlib)
endif()

# Each module directory owns its CMakeLists.txt. Portable modules define a
# static library ecg_<name> and, when NOT ECG_TARGET, their own tests.
foreach(mod dsp drivers ui acq app)
  if(EXISTS ${CMAKE_SOURCE_DIR}/fw/${mod}/CMakeLists.txt)
    add_subdirectory(fw/${mod})
  endif()
endforeach()

if(ECG_TARGET)
  if(EXISTS ${CMAKE_SOURCE_DIR}/fw/bsp/CMakeLists.txt)
    add_subdirectory(fw/bsp)      # defines ecg_bsp and the firmware executable
  endif()
else()
  if(EXISTS ${CMAKE_SOURCE_DIR}/host/sim/CMakeLists.txt)
    add_subdirectory(host/sim)    # defines ecg_sim (hal.h on the host)
  endif()
  add_subdirectory(host/radio_tests)
  # The BSP's pure index arithmetic (no TI headers) is tested on the host.
  ecg_add_test(bsp_ring SOURCES fw/bsp/tests/test_bsp_ring.c)
  target_include_directories(bsp_ring PRIVATE ${CMAKE_SOURCE_DIR}/fw/bsp)
endif()
