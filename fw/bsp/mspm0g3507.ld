/* MSPM0G3507 (128 KB flash, 32 KB SRAM) linker script for the ECG card.
 *
 * The last 1 KB flash sector (0x0001FC00..0x0001FFFF) is NOT part of FLASH:
 * it is the hal_nv_* settings sector and is never touched by the image
 * (J-Link only erases sectors the image occupies). */

ENTRY(Reset_Handler)
EXTERN(g_vectors)

__stack_size = DEFINED(__stack_size) ? __stack_size : 0x800;

MEMORY
{
    FLASH   (rx)  : ORIGIN = 0x00000000, LENGTH = 0x0001FC00
    NVSTORE (r)   : ORIGIN = 0x0001FC00, LENGTH = 0x00000400
    SRAM    (rwx) : ORIGIN = 0x20200000, LENGTH = 0x00008000
}

__nv_start = ORIGIN(NVSTORE);
__nv_end   = ORIGIN(NVSTORE) + LENGTH(NVSTORE);

SECTIONS
{
    .intvecs 0x00000000 : {
        KEEP(*(.intvecs))
    } > FLASH

    .text : ALIGN(4) {
        *(.text .text.*)
        *(.glue_7 .glue_7t .vfp11_veneer .v4_bx)
        KEEP(*(.init))
        KEEP(*(.fini))
    } > FLASH

    .rodata : ALIGN(4) {
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > FLASH

    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > FLASH
    .ARM.exidx : {
        __exidx_start = .;
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        __exidx_end = .;
    } > FLASH

    .preinit_array : ALIGN(4) {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array*))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } > FLASH
    .init_array : ALIGN(4) {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > FLASH
    .fini_array : ALIGN(4) {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array*))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } > FLASH

    /* Code that must run while the flash bank is busy (DriverLib flashctl
     * *FromRAM). Copied to SRAM by Reset_Handler. */
    .ramfunc : ALIGN(8) {
        __ramfunc_start__ = .;
        *(.ramfunc .ramfunc.*)
        . = ALIGN(8);
        __ramfunc_end__ = .;
    } > SRAM AT > FLASH
    __ramfunc_load__ = LOADADDR(.ramfunc);

    .data : ALIGN(8) {
        __data_start__ = .;
        *(.data .data.*)
        . = ALIGN(8);
        __data_end__ = .;
    } > SRAM AT > FLASH
    __data_load__ = LOADADDR(.data);

    .bss (NOLOAD) : ALIGN(8) {
        __bss_start__ = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(8);
        __bss_end__ = .;
    } > SRAM

    .noinit (NOLOAD) : ALIGN(8) {
        *(.noinit .noinit.*)
        . = ALIGN(8);
    } > SRAM

    /* No heap (no malloc in fw/). newlib-nano still wants the symbols. */
    __ram_used_end = .;
    PROVIDE(end = .);
    PROVIDE(_end = .);

    /* Main stack: the top __stack_size bytes of SRAM (full descending). */
    __StackTop = ORIGIN(SRAM) + LENGTH(SRAM);
    __StackLimit = __StackTop - __stack_size;
    ASSERT(__StackLimit >= __ram_used_end,"SRAM overflow: .data + .bss + stack > 32 KB")

    __flash_used = LOADADDR(.data) + SIZEOF(.data);
    ASSERT(__flash_used <= ORIGIN(NVSTORE), "image overlaps the NV sector")
}
