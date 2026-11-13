add_library(ecg_sim STATIC sim_core.c sim_i2c.c sim_oled.c sim_analog.c sim_radio.c)
target_include_directories(ecg_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ecg_sim PUBLIC ecg_iface m)

if(NOT ECG_TARGET)
  foreach(t time_events invariants i2c oled ecg ppg nv_misc radio)
    ecg_add_test(sim_${t} SOURCES tests/test_${t}.c LIBS ecg_sim)
  endforeach()
endif()

# Default mode must abort loudly on a violation (the child prints the message and aborts).
if(NOT ECG_TARGET)
  add_test(NAME sim_invariants_abort
    COMMAND sh -c "$0 die 2>&1 | grep -q 'SIM VIOLATION'" $<TARGET_FILE:sim_invariants>
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
