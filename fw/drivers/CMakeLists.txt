add_library(ecg_drivers STATIC
  src/ssd1306.c
  src/mcp4728.c
  src/cw2015.c
  src/charger.c
  src/buttons.c)
target_include_directories(ecg_drivers PUBLIC include PRIVATE src)
target_link_libraries(ecg_drivers PUBLIC ecg_iface)

if(NOT ECG_TARGET)
  set(MOCK tests/mock_hal.c tests/i2c_models.c)
  foreach(t ssd1306 mcp4728 cw2015 charger buttons)
    ecg_add_test(drivers_${t} SOURCES tests/test_${t}.c ${MOCK} LIBS ecg_drivers)
    target_include_directories(drivers_${t} PRIVATE tests)
  endforeach()
endif()
