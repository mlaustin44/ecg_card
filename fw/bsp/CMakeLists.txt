# fw/bsp: hal.h on the MSPM0G3507 (TI DriverLib), startup code, linker
# script and the firmware executables. Only added when cross-compiling.

if(NOT DEFINED MSPM0_SDK_ROOT OR NOT EXISTS "${MSPM0_SDK_ROOT}/source/ti/driverlib")
  message(FATAL_ERROR "fw/bsp: MSPM0_SDK_ROOT (${MSPM0_SDK_ROOT}) is not a TI MSPM0 SDK")
endif()

set(ECG_BSP_LDS ${CMAKE_CURRENT_SOURCE_DIR}/mspm0g3507.ld)
set(ECG_DRIVERLIB
    ${MSPM0_SDK_ROOT}/source/ti/driverlib/lib/gcc/m0p/mspm0g1x0x_g3x0x/driverlib.a)
if(NOT EXISTS ${ECG_DRIVERLIB})
  message(FATAL_ERROR "fw/bsp: prebuilt DriverLib not found: ${ECG_DRIVERLIB}")
endif()

# TI headers as SYSTEM includes: -Werror and the warning set apply to our
# code only. Only fw/bsp sees them.
add_library(ecg_mspm0_sdk INTERFACE)
target_include_directories(ecg_mspm0_sdk SYSTEM INTERFACE
  ${MSPM0_SDK_ROOT}/source
  ${MSPM0_SDK_ROOT}/source/third_party/CMSIS/Core/Include)
target_compile_definitions(ecg_mspm0_sdk INTERFACE __MSPM0G3507__)

# ecg_bsp is an OBJECT library: its objects (vector table, ISRs, hal_*) are
# always on the link line, so (a) the strong ISR definitions are never lost
# to archive member selection and (b) hal_* symbols resolve regardless of the
# order in which ecg_app / ecg_drivers / ... appear. --gc-sections still
# drops what nothing references.
add_library(ecg_bsp OBJECT
  startup_mspm0g3507.c
  hal_core.c
  hal_bus.c
  hal_acq.c
  hal_nv.c
  rtt.c)
target_link_libraries(ecg_bsp
  PUBLIC ecg_iface
  PRIVATE ecg_mspm0_sdk)
target_link_libraries(ecg_bsp INTERFACE ${ECG_DRIVERLIB})
target_link_options(ecg_bsp INTERFACE
  -nostartfiles
  -T${ECG_BSP_LDS}
  -Wl,--print-memory-usage
  -Wl,--no-warn-rwx-segments) # .ramfunc (flash command loop) is code in SRAM

# Firmware image: <name>.elf plus .hex/.bin/.map, size printed after linking.
function(ecg_firmware name)
  add_executable(${name} ${ARGN})
  set_target_properties(${name} PROPERTIES SUFFIX ".elf" LINK_DEPENDS ${ECG_BSP_LDS})
  target_link_libraries(${name} PRIVATE ecg_bsp ecg_iface)
  target_link_options(${name} PRIVATE "-Wl,-Map=$<TARGET_FILE_DIR:${name}>/${name}.map")
  add_custom_command(TARGET ${name} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${name}> $<TARGET_FILE_DIR:${name}>/${name}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${name}> $<TARGET_FILE_DIR:${name}>/${name}.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${name}>
    VERBATIM)
endfunction()

ecg_firmware(ecg_bsp_smoke smoke_main.c)

if(TARGET ecg_app)
  ecg_firmware(ecg_fw main.c)
  target_link_libraries(ecg_fw PRIVATE ecg_app)
endif()
