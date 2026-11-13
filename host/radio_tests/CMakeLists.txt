# Host tests of the CH585 radio firmware's portable modules (CH585F/APP/rl_*.c).
# The WCH-specific sources (main.c, uart_link.c, link_mgr.c, ble_app.c,
# radio_board.c, Profile/) are not compiled here: they need the WCH SDK and
# toolchain (scripts/build_ch585.sh, or MounRiver Studio).
set(RADIO_APP ${CMAKE_SOURCE_DIR}/CH585F/APP)

add_library(ecg_radio_portable STATIC
  ${RADIO_APP}/rl_codec.c
  ${RADIO_APP}/rl_parser.c
  ${RADIO_APP}/rl_msg.c
  ${RADIO_APP}/rl_queue.c
  ${RADIO_APP}/rl_fmt.c
  ${RADIO_APP}/rl_ble_fmt.c)
# The radio's own copies of link_fmt.h / stream_fmt.h, ahead of fw/include.
target_include_directories(ecg_radio_portable BEFORE PUBLIC ${RADIO_APP}/include)

foreach(t codec parser msg queue fmt)
  ecg_add_test(radio_${t} SOURCES test_${t}.c LIBS ecg_radio_portable)
endforeach()

# Frames from the MSPM0 encoder (fw/app/src/stream.c, built as-is with a
# capturing hal_stream_write) through the radio parser, and the reverse.
ecg_add_test(radio_cross_mspm0 SOURCES test_cross_mspm0.c ${CMAKE_SOURCE_DIR}/fw/app/src/stream.c
             LIBS ecg_radio_portable)
target_include_directories(radio_cross_mspm0 PRIVATE ${CMAKE_SOURCE_DIR}/fw/app/src)

# CH585F/APP/include keeps byte-identical copies of the contract headers.
foreach(h link_fmt.h stream_fmt.h)
  add_test(NAME radio_header_${h}
    COMMAND ${CMAKE_COMMAND} -E compare_files
            ${CMAKE_SOURCE_DIR}/fw/include/${h} ${RADIO_APP}/include/${h})
endforeach()
