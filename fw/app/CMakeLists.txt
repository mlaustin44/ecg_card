# ecg_app: state machine, navigation, power policy, settings, stream output,
# diagnostics and app_main(). Depends on everything below it.
add_library(ecg_app STATIC
  src/app.c
  src/nav.c
  src/settings_store.c
  src/stream.c
  src/applog.c
  src/diag.c)
target_include_directories(ecg_app PUBLIC include PRIVATE src)
target_link_libraries(ecg_app PUBLIC ecg_iface ecg_acq ecg_ui ecg_drivers ecg_dsp)
# Open question 10 of the brief: blank the OLED during PPG captures.
option(APP_PPG_BLANK_DISPLAY "Blank the OLED (AEh) while PPG is RUNNING" OFF)
if(APP_PPG_BLANK_DISPLAY)
  target_compile_definitions(ecg_app PRIVATE APP_PPG_BLANK_DISPLAY)
endif()

if(NOT ECG_TARGET)
  set(APP_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/build/app_shots)

  # Unit tests of the pure pieces.
  foreach(t stream settings nav)
    ecg_add_test(app_unit_${t} SOURCES tests/test_${t}.c LIBS ecg_app ecg_sim)
    target_include_directories(app_unit_${t} PRIVATE src)
  endforeach()

  # System scenarios: the whole application against ecg_sim in ABORT mode.
  add_library(ecg_app_scen STATIC tests/scen.c)
  target_include_directories(ecg_app_scen PUBLIC tests src)
  target_link_libraries(ecg_app_scen PUBLIC ecg_app ecg_testsrc ecg_sim ecg_testlib m)
  foreach(t ui ecg usb ppg both power faults)
    ecg_add_test(app_scen_${t} SOURCES tests/scen_${t}.c LIBS ecg_app_scen)
  endforeach()
  set_tests_properties(app_scen_both PROPERTIES FIXTURES_SETUP app_capture)

  # The same application built with the display-blanking option.
  add_executable(app_scen_blank tests/scen_blank.c tests/scen.c
    src/app.c src/nav.c src/settings_store.c src/stream.c src/applog.c src/diag.c)
  target_compile_definitions(app_scen_blank PRIVATE APP_PPG_BLANK_DISPLAY)
  target_include_directories(app_scen_blank PRIVATE include src tests)
  target_link_libraries(app_scen_blank PRIVATE ecg_acq ecg_ui ecg_drivers ecg_dsp ecg_testsrc
                        ecg_sim ecg_testlib ecg_iface m)
  add_test(NAME app_scen_blank COMMAND app_scen_blank WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

  # Python decoder on the capture saved by app_scen_both (skipped without .venv).
  add_test(NAME app_capture_python
    COMMAND sh -c "if [ -x .venv/bin/python ]; then exec .venv/bin/python tools/check_capture.py build/app_capture.bin --min-ecg-blocks 1000 --min-ppg-blocks 1000 --require-events r_peak,pulse,lead,state; else echo 'no .venv: skipped'; exit 77; fi"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set_tests_properties(app_capture_python PROPERTIES FIXTURES_REQUIRED app_capture SKIP_RETURN_CODE 77)
endif()
