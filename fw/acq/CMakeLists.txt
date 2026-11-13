# ecg_acq: rail sequencing (rails.c, the only owner of the analog enables)
# and the ECG / PPG acquisition engine (acq.c).
add_library(ecg_acq STATIC
  src/rails.c
  src/acq.c)
target_include_directories(ecg_acq PUBLIC include)
target_link_libraries(ecg_acq PUBLIC ecg_iface ecg_drivers ecg_dsp)

if(NOT ECG_TARGET)
  # Against the host simulator in ABORT mode: any rail-rule violation fails.
  # Synthetic ECG + PPG world on the simulator (also used by fw/app tests).
  add_library(ecg_testsrc STATIC tests/testsrc.c)
  target_include_directories(ecg_testsrc PUBLIC tests)
  target_link_libraries(ecg_testsrc PUBLIC ecg_dsp_synth ecg_sim m)

  ecg_add_test(acq_rails SOURCES tests/test_rails.c LIBS ecg_acq ecg_sim)
  ecg_add_test(acq_engine SOURCES tests/test_acq.c LIBS ecg_acq ecg_testsrc ecg_sim)
  ecg_add_test(acq_stats SOURCES tests/test_stats.c)
  target_include_directories(acq_stats PRIVATE src)
endif()
