/* Golden frame hashes; regenerate with UI_GOLDEN_PRINT=1 (see test_golden.c). */
static const golden_t golden[] = {
    {"home", 0x6842F6C2u},
    {"card_ecg", 0xBB811248u},
    {"card_both", 0x464C7226u},
    {"settings_2", 0x422050B9u}, /* scroll arrow: Settings has 8 rows */
    {"about", 0x2118DB3Cu},
    {"about_radio", 0x5C05DB91u},
    {"settings_5", 0x46981B81u}, /* down arrow: Restore below */
    {"settings_6", 0xC9F6C0BFu}, /* down arrow: Live view below */
    {"settings_7", 0x1DADADEAu},
    {"settings_exit", 0xB3CF4A72u},
    {"settings_closed", 0x9AB3E515u},
    {"ecg_run_hr", 0xE2A855B7u},
    {"ppg_run_hr", 0xF96DD051u},
    {"home_ble_adv", 0x8B7EA942u},
    {"home_ble_conn", 0x27E0172Cu},
    {"card_ppg_ble_fault_usb", 0xB4523A80u},
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
    {"card_hrm", 0x11838A47u},   /* HR monitor card (7 page dots) */
    {"card_hrm_ble_off", 0x7FC6C2E6u},
    {"hrm_wait", 0x8513A48Eu},
    {"hrm_settling", 0x36B2603Du},
    {"hrm_run", 0xFC71A5E9u},    /* big rate, Bluetooth row, session time */
    {"hrm_run_off", 0x1D7C9E51u}, /* Bluetooth off: turn it on */
};
