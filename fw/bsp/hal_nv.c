/* hal.h non-volatile blob in the last 1 KB flash sector (0x0001FC00), which
 * the linker script removes from the FLASH region. */
#include <string.h>

#include "bsp_internal.h"

extern const uint8_t __nv_start[]; /* linker: ORIGIN(NVSTORE) */
#define NV_ADDR ((uint32_t) __nv_start)
#define NV_WORD 8u                 /* flash word: 64 data bits + ECC */

_Static_assert(HAL_NV_MAX % NV_WORD == 0u, "whole flash words");
_Static_assert(HAL_NV_MAX <= 1024u, "fits the sector");

void hal_nv_read(void *dst, size_t n)
{
    if (dst == NULL) return;
    if (n > HAL_NV_MAX) {
        memset((uint8_t *) dst + HAL_NV_MAX, 0xFF, n - HAL_NV_MAX);
        n = HAL_NV_MAX;
    }
    memcpy(dst, (const void *) NV_ADDR, n);
}

/* Each command: clear status, unprotect the sector (protection re-arms after
 * every command), execute from RAM (DriverLib *FromRAM: the wait loop runs in
 * .ramfunc because the single flash bank is busy). IRQs are masked so no ISR
 * fetches from flash during the operation. */
static bool nv_erase(void)
{
    uint32_t pm = hal_irq_lock();
    DL_FlashCTL_executeClearStatus(FLASHCTL);
    DL_FlashCTL_unprotectSector(FLASHCTL, NV_ADDR, DL_FLASHCTL_REGION_SELECT_MAIN);
    DL_FLASHCTL_COMMAND_STATUS st =
        DL_FlashCTL_eraseMemoryFromRAM(FLASHCTL, NV_ADDR, DL_FLASHCTL_COMMAND_SIZE_SECTOR);
    hal_irq_unlock(pm);
    return st == DL_FLASHCTL_COMMAND_STATUS_PASSED;
}

static bool nv_program_word(uint32_t addr, const uint32_t data[2])
{
    uint32_t pm = hal_irq_lock();
    DL_FlashCTL_executeClearStatus(FLASHCTL);
    DL_FlashCTL_unprotectSector(FLASHCTL, addr, DL_FLASHCTL_REGION_SELECT_MAIN);
    DL_FLASHCTL_COMMAND_STATUS st =
        DL_FlashCTL_programMemoryFromRAM64WithECCGenerated(FLASHCTL, addr, data);
    hal_irq_unlock(pm);
    return st == DL_FLASHCTL_COMMAND_STATUS_PASSED;
}

hal_status_t hal_nv_write(const void *src, size_t n)
{
    const uint8_t *s = src;
    uint32_t word[2];
    size_t off;

    if (n > HAL_NV_MAX || (n != 0u && src == NULL)) return HAL_ERR_PARAM;
    if (n != 0u && memcmp((const void *) NV_ADDR, src, n) == 0) return HAL_OK;

    hal_wdt_kick();
    if (!nv_erase()) return HAL_ERR_TIMEOUT;
    for (off = 0; off < n; off += NV_WORD) {
        size_t c = n - off < NV_WORD ? n - off : NV_WORD;
        memset(word, 0xFF, sizeof word);
        memcpy(word, s + off, c);
        if (!nv_program_word(NV_ADDR + off, word)) return HAL_ERR_TIMEOUT;
    }
    hal_wdt_kick();
    if (n != 0u && memcmp((const void *) NV_ADDR, src, n) != 0) return HAL_ERR_BUS;
    return HAL_OK;
}
