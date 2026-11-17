/* Golden frame hashes; regenerate with UI_GOLDEN_PRINT=1 (see test_golden.c). */
static const golden_t golden[] = {
    {"home", 0xB283D73Au},
    {"card_ecg", 0xA97519D0u},
    {"card_both", 0xA81F321Eu},
    {"settings_2", 0x422050B9u}, /* scroll arrow: Settings has 8 rows */
    {"about", 0x4AE6F624u},
    {"about_radio", 0x28B7B439u},
    {"settings_5", 0x46981B81u}, /* down arrow: Restore below */
    {"settings_6", 0xC9F6C0BFu}, /* down arrow: Live view below */
    {"settings_7", 0x1DADADEAu},
    {"settings_exit", 0xB3CF4A72u},
    {"settings_closed", 0x0246485Du},
    {"ecg_run_hr", 0xE2A855B7u},
    {"ppg_run_hr", 0xF96DD051u},
    {"home_ble_adv", 0xD5BF89BAu},
    {"home_ble_conn", 0x025A0DD4u},
    {"card_ppg_ble_fault_usb", 0x67FBCD68u},
    {"diag", 0xD759853Bu},
    {"ecg_wait_c", 0x0E76A85Au},
    {"ecg_settling", 0x2CE9BD66u},
    {"ecg_run", 0x8326C9F3u},
    {"ecg_run_205", 0x92C4AE7Au},
    {"ppg_run", 0xA263A677u},
    {"both_run", 0x82F250EBu},
    {"sum_ecg", 0xB581B64Cu},
    {"sum_both", 0x83AC6C04u},
    {"ov_power_50", 0x34AF89F1u},
    {"ov_fault", 0xAEC639F2u},
    {"boot", 0xDD0E9E3Cu},       /* designer credit after reset / button wake */
    {"boot_t500", 0x311E815Bu},  /* trace half drawn */
    {"power_down", 0x82B88BCBu}, /* designer credit before STANDBY */
};
