cmake_minimum_required(VERSION 3.20)
project(ecg_card C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Werror -Wshadow -Wdouble-promotion -Wundef)

# Portable interface headers: board.h, hal.h, settings.h, ui_model.h, fw_config.h
add_library(ecg_iface INTERFACE)
target_include_directories(ecg_iface INTERFACE ${CMAKE_SOURCE_DIR}/fw/include)

if(CMAKE_CROSSCOMPILING)
  set(ECG_TARGET ON)
else()
  set(ECG_TARGET OFF)
  option(ECG_SANITIZE "Build host code with ASan/UBSan" ON)
  if(ECG_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
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
endif()
