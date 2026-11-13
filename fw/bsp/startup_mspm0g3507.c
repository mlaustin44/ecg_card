/* MSPM0G3507 reset handler and vector table (GCC). Vector order follows the
 * device header's IRQn_Type (mspm0g350x.h).
 *
 * The handlers the BSP implements are referenced by name (strong), so the
 * linker always pulls them in; every other vector points at a fault loop.
 * The table is kept with EXTERN() + KEEP() in the linker script. */
#include <stdint.h>

#include "bsp_internal.h"

extern uint32_t __data_load__, __data_start__, __data_end__;
extern uint32_t __ramfunc_load__, __ramfunc_start__, __ramfunc_end__;
extern uint32_t __bss_start__, __bss_end__;
extern uint32_t __StackTop;

int main(void);
void Reset_Handler(void);
void Unused_Handler(void);
void HardFault_Handler(void);
void NMI_Handler(void);
void SysTick_Handler(void);
void GROUP1_IRQHandler(void);
void ADC0_IRQHandler(void);
void ADC1_IRQHandler(void);
void RTC_IRQHandler(void);
void UART0_IRQHandler(void);

typedef void (*vector_t)(void);

#define U Unused_Handler
__attribute__((used, section(".intvecs")))
const vector_t g_vectors[16 + 32] = {
    (vector_t) &__StackTop,
    Reset_Handler,
    NMI_Handler,
    HardFault_Handler,
    0, 0, 0, 0, 0, 0, 0,
    U,                 /* SVCall */
    0, 0,
    U,                 /* PendSV */
    SysTick_Handler,
    /* IRQ 0..31 */
    U,                 /*  0 GROUP0: SYSCTL, WWDT0/1, FLASHCTL, DEBUGSS */
    GROUP1_IRQHandler, /*  1 GROUP1: GPIOA, GPIOB, COMP0..2, TRNG */
    U,                 /*  2 TIMG8 */
    U,                 /*  3 UART3 */
    ADC0_IRQHandler,   /*  4 ADC0 */
    ADC1_IRQHandler,   /*  5 ADC1 */
    U,                 /*  6 CANFD0 */
    U,                 /*  7 DAC0 */
    0,                 /*  8 reserved */
    U,                 /*  9 SPI0 */
    U,                 /* 10 SPI1 */
    0, 0,              /* 11, 12 reserved */
    U,                 /* 13 UART1 */
    U,                 /* 14 UART2 */
    UART0_IRQHandler,  /* 15 UART0 */
    U,                 /* 16 TIMG0 */
    U,                 /* 17 TIMG6 */
    U,                 /* 18 TIMA0 */
    U,                 /* 19 TIMA1 */
    U,                 /* 20 TIMG7 */
    U,                 /* 21 TIMG12 */
    0, 0,              /* 22, 23 reserved */
    U,                 /* 24 I2C0 */
    U,                 /* 25 I2C1 */
    0, 0,              /* 26, 27 reserved */
    U,                 /* 28 AES */
    0,                 /* 29 reserved */
    RTC_IRQHandler,    /* 30 RTC */
    U,                 /* 31 DMA */
};
#undef U

void Reset_Handler(void)
{
    uint32_t *src, *dst;

    for (src = &__data_load__, dst = &__data_start__; dst < &__data_end__;)
        *dst++ = *src++;
    for (src = &__ramfunc_load__, dst = &__ramfunc_start__; dst < &__ramfunc_end__;)
        *dst++ = *src++;
    for (dst = &__bss_start__; dst < &__bss_end__;)
        *dst++ = 0u;

    (void) main();
    for (;;) {
    }
}

/* Faults and unexpected interrupts: make the board safe (rails and switches
 * off, LEDs blanked), then wait for the 2 s watchdog. A debugger can inspect
 * the state in the meantime. */
void HardFault_Handler(void)
{
    bsp_safe_state();
    for (;;) {
    }
}

void NMI_Handler(void)
{
    HardFault_Handler();
}

void Unused_Handler(void)
{
    HardFault_Handler();
}
