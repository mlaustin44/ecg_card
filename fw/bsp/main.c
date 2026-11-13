/* Firmware entry: bring the board to its safe idle state, then hand over to
 * the application. app_main() never returns; if it ever does, the watchdog
 * (not kicked here) resets the card within about 2 s. */
#include "hal.h"

void app_main(void);

int main(void)
{
    hal_init(); /* idempotent: app_main() may call it again */
    app_main();
    for (;;) {
    }
}
